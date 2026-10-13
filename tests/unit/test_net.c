/*
 * test_net.c — testes do send_all_timeout() (src/common/net.c).
 *
 * É a peça que impede um cliente que não lê de congelar o servidor (M2,
 * slide 125: "A slow or stuck client blocks everyone. Use timeouts").
 * Usa um socketpair(): dois sockets ligados um ao outro, sem rede nem portas,
 * e onde controlamos se o outro lado lê ou não.
 *
 *   53  caso normal: tudo enviado, bytes iguais do outro lado
 *   54  o outro lado não lê: devolve SEND_TIMEOUT_EXPIRED, e no prazo
 *   55  o prazo é TOTAL, não por send(): um leitor lento não o prolonga
 *   56  o outro lado fechou: -1 (EPIPE), e o processo não morre com SIGPIPE
 *   57  depois de expirar pode ter saído MEIA mensagem (typical-problems §1.3)
 */
#include "net.h"
#include "test_util.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Mais do que cabe nos buffers do kernel de um socketpair (algumas centenas
 * de KB), para garantir que o envio fica mesmo à espera. */
#define BIG (8u * 1024 * 1024)

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void caso_53_normal(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    const char msg[] = "{\"type\":\"OK\"}\n";
    CASE(53, "linha curta: enviada por inteiro");
    CHECK_INT(send_all_timeout(sv[0], msg, sizeof msg - 1, 1000), 0);

    char got[64];
    ssize_t n = recv(sv[1], got, sizeof got, 0);
    CASE(53, "do outro lado chegam exatamente os mesmos bytes");
    CHECK_INT(n, (long)(sizeof msg - 1));
    CHECK_MEM(got, msg, sizeof msg - 1);

    close(sv[0]);
    close(sv[1]);
}

static void caso_54_nao_le(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    char *big = calloc(1, BIG);
    CHECK(big != NULL);
    if (big == NULL)
        return;

    long t0 = now_ms();
    int  rc = send_all_timeout(sv[0], big, BIG, 300);
    long dt = now_ms() - t0;

    CASE(54, "ninguem le: devolve SEND_TIMEOUT_EXPIRED");
    CHECK_INT(rc, SEND_TIMEOUT_EXPIRED);
    CASE(54, "e desiste perto do prazo (300 ms), nao fica preso");
    CHECK(dt >= 250);
    CHECK(dt < 2000);

    free(big);
    close(sv[0]);
    close(sv[1]);
}

/* Leitor "a conta-gotas": tira 64 KB de 100 em 100 ms. Com um prazo POR
 * send() (é o que o SO_SNDTIMEO faz) cada bocado libertado deixava fazer mais
 * um send() e o prazo recomeçava: os 8 MB acabavam por sair ao fim de ~13 s e
 * a thread do servidor ficava presa esse tempo todo. Com o prazo total tem de
 * expirar perto dos 400 ms na mesma. (Mutação verificada: recomeçar o prazo a
 * cada send() faz este caso falhar.) */
static _Atomic int pinga_parar = 0;

static void *pinga(void *arg)
{
    int  fd = *(int *)arg;
    static char b[64 * 1024];
    while (!pinga_parar) {
        if (recv(fd, b, sizeof b, MSG_DONTWAIT) < 0 && errno != EAGAIN)
            break;
        struct timespec d = { .tv_sec = 0, .tv_nsec = 100 * 1000000L };
        nanosleep(&d, NULL);
    }
    return NULL;
}

static void caso_55_prazo_total(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    char *big = calloc(1, BIG);
    CHECK(big != NULL);
    if (big == NULL)
        return;

    pthread_t th;
    pinga_parar = 0;
    CHECK_INT(pthread_create(&th, NULL, pinga, &sv[1]), 0);

    long t0 = now_ms();
    int  rc = send_all_timeout(sv[0], big, BIG, 400);
    long dt = now_ms() - t0;

    pinga_parar = 1;
    pthread_join(th, NULL);

    CASE(55, "leitor lento: o prazo expira na mesma");
    CHECK_INT(rc, SEND_TIMEOUT_EXPIRED);
    CASE(55, "e o prazo e TOTAL: ~400 ms, nao 400 ms por cada send()");
    CHECK(dt < 2000);

    free(big);
    close(sv[0]);
    close(sv[1]);
}

static void caso_56_peer_fechou(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    close(sv[1]);

    /* Se o MSG_NOSIGNAL faltasse, o processo morria aqui com SIGPIPE e o
     * make test acusava a falha pelo código de saída. */
    const char msg[] = "x\n";
    CASE(56, "peer fechado: -1 (EPIPE), sem SIGPIPE");
    CHECK_INT(send_all_timeout(sv[0], msg, 2, 1000), -1);

    close(sv[0]);
}

static void caso_57_meia_mensagem(void)
{
    int sv[2];
    CHECK_INT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    char *big = calloc(1, BIG);
    CHECK(big != NULL);
    if (big == NULL)
        return;

    send_all_timeout(sv[0], big, BIG, 200);

    /* Quanto chegou ao outro lado? Ler tudo o que lá está, sem esperar. */
    size_t  total = 0;
    char    buf[65536];
    ssize_t n;
    while ((n = recv(sv[1], buf, sizeof buf, MSG_DONTWAIT)) > 0)
        total += (size_t)n;

    /* É por isto que o servidor, depois de um envio expirar, desliga o
     * cliente em vez de continuar: o que ficou no fio é meia "linha", e o
     * próximo '\n' apareceria no sítio errado. */
    CASE(57, "expirou com PARTE dos bytes ja no outro lado");
    CHECK(total > 0);
    CHECK(total < BIG);

    free(big);
    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    caso_53_normal();
    caso_54_nao_le();
    caso_55_prazo_total();
    caso_56_peer_fechou();
    caso_57_meia_mensagem();
    return t_report("net");
}
