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
 *
 * M3 (slide 85, "Use getaddrinfo() and sockaddr_storage for IPv4 / IPv6
 * portability"):
 *   76  addr_to_str(): IPv4, IPv6 e IPv4 que chega por um socket dual-stack
 *   77  tcp_listen() aceita IPv4 E IPv6 no mesmo socket — com ligações reais
 *       em 127.0.0.1 e em ::1. Numa máquina sem IPv6 verifica o recurso a
 *       IPv4 e diz que saltou a parte IPv6.
 *
 * M4:
 *   90  wait_readable(): prazo, bytes, FIN e o pipe de paragem (Ctrl+C)
 *   91  linger_drain(): o FIN sai depois do que já foi enviado, o que o peer
 *       ainda manda é lido e deitado fora, e há um prazo
 */
#include "net.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/wait.h>
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

/* ---- M3: IPv4 + IPv6 ------------------------------------------------------ */

static void caso_76_addr_to_str(void)
{
    char out[64];

    CASE(76, "IPv4: a.b.c.d:porta");
    struct sockaddr_in a4 = { .sin_family = AF_INET, .sin_port = htons(5000) };
    inet_pton(AF_INET, "192.168.1.7", &a4.sin_addr);
    addr_to_str((struct sockaddr *)&a4, out, sizeof out);
    CHECK_STR(out, "192.168.1.7:5000");

    CASE(76, "IPv6: [endereco]:porta (os ':' nao se confundem com a porta)");
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6, .sin6_port = htons(5000) };
    inet_pton(AF_INET6, "::1", &a6.sin6_addr);
    addr_to_str((struct sockaddr *)&a6, out, sizeof out);
    CHECK_STR(out, "[::1]:5000");

    CASE(76, "IPv4 num socket dual-stack (::ffff:a.b.c.d) mostra-se como IPv4");
    inet_pton(AF_INET6, "::ffff:10.0.0.2", &a6.sin6_addr);
    addr_to_str((struct sockaddr *)&a6, out, sizeof out);
    CHECK_STR(out, "10.0.0.2:5000");
}

/* Liga a host:port do listener, aceita e devolve o "who" do lado do servidor
 * (o que o servidor escreve nos logs). "" se não ligou. */
static void liga_e_aceita(int lfd, const char *host, const char *port, char *who, size_t cap)
{
    who[0] = '\0';
    int c = tcp_connect(host, port);
    if (c < 0)
        return;
    struct sockaddr_storage peer;
    socklen_t len = sizeof peer;
    int s = accept(lfd, (struct sockaddr *)&peer, &len);
    if (s >= 0) {
        addr_to_str((struct sockaddr *)&peer, who, cap);
        close(s);
    }
    close(c);
}

static void caso_77_dual_stack(void)
{
    /* Porta "0": o kernel escolhe uma livre, para o teste nunca colidir com
     * um servidor que esteja a correr. */
    int lfd = tcp_listen("0", 4);
    CASE(77, "tcp_listen(\"0\") cria o socket");
    CHECK(lfd >= 0);
    if (lfd < 0)
        return;

    struct sockaddr_storage me;
    socklen_t len = sizeof me;
    getsockname(lfd, (struct sockaddr *)&me, &len);
    unsigned p = me.ss_family == AF_INET6
               ? ntohs(((struct sockaddr_in6 *)&me)->sin6_port)
               : ntohs(((struct sockaddr_in *)&me)->sin_port);
    char port[8], who[64];
    snprintf(port, sizeof port, "%u", p);

    CASE(77, "um cliente IPv4 (127.0.0.1) liga, e aparece como 127.0.0.1");
    liga_e_aceita(lfd, "127.0.0.1", port, who, sizeof who);
    CHECK(strncmp(who, "127.0.0.1:", 10) == 0);

    if (me.ss_family == AF_INET6) {
        CASE(77, "o MESMO socket aceita um cliente IPv6 (::1), mostrado como [::1]");
        liga_e_aceita(lfd, "::1", port, who, sizeof who);
        CHECK(strncmp(who, "[::1]:", 6) == 0);
    } else {
        /* Máquina sem IPv6 (ex.: alguns contentores): o recurso a IPv4 é o
         * comportamento certo. Diz-se em vez de falhar ou fingir que passou. */
        printf("  (sem IPv6 nesta maquina: tcp_listen caiu para IPv4; parte IPv6 saltada)\n");
    }
    close(lfd);
}

/* ---- M4 ---------------------------------------------------------------- */

static void caso_90_wait_readable(void)
{
    int sv[2], stop[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (pipe(stop) != 0)
        return;

    CASE(90, "nada para ler: NET_TIMEOUT, e so depois do prazo");
    long t0 = now_ms();
    CHECK_INT(wait_readable(sv[0], stop[0], 150), NET_TIMEOUT);
    long dt = now_ms() - t0;
    CHECK(dt >= 140 && dt < 1000);

    CASE(90, "prazo 0: nao espera");
    CHECK_INT(wait_readable(sv[0], -1, 0), NET_TIMEOUT);

    CASE(90, "bytes: NET_READY");
    CHECK_INT(write(sv[1], "x", 1), 1);
    CHECK_INT(wait_readable(sv[0], stop[0], 1000), NET_READY);
    char c;
    CHECK_INT(read(sv[0], &c, 1), 1);

    CASE(90, "o pipe de paragem tem prioridade, mesmo com bytes a espera");
    CHECK_INT(write(sv[1], "y", 1), 1);
    CHECK_INT(write(stop[1], "s", 1), 1);
    CHECK_INT(wait_readable(sv[0], stop[0], 1000), NET_STOP);
    CHECK_INT(wait_readable(sv[0], stop[0], -1), NET_STOP);   /* continua (nunca se le) */

    CASE(90, "o FIN do outro lado tambem acorda (o recv() a seguir da 0)");
    close(stop[0]); close(stop[1]);
    CHECK_INT(read(sv[0], &c, 1), 1);
    close(sv[1]);
    CHECK_INT(wait_readable(sv[0], -1, 1000), NET_READY);
    CHECK_INT(recv(sv[0], &c, 1, 0), 0);
    close(sv[0]);
}

static void caso_91_linger_drain(void)
{
    int sv[2];
    char buf[64];

    CASE(91, "o que ja tinhamos enviado chega ANTES do nosso FIN; o lixo do peer e lido");
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    CHECK_INT(write(sv[0], "ultima\n", 7), 7);           /* a nossa ultima resposta */
    CHECK_INT(write(sv[1], "lixo por ler", 12), 12);      /* o peer ainda mandou mais */
    long t0 = now_ms();
    pid_t pid = fork();
    if (pid == 0) {                       /* o peer: le tudo ate ao FIN, e fecha */
        close(sv[0]);
        char b[64];
        ssize_t n, tot = 0;
        while ((n = read(sv[1], b + tot, sizeof b - (size_t)tot)) > 0)
            tot += n;
        _exit(tot == 7 && memcmp(b, "ultima\n", 7) == 0 ? 0 : 1);
    }
    close(sv[1]);                         /* so o filho fica com o lado do peer */
    CHECK_INT(linger_drain(sv[0], 2000), 0);              /* leu o lixo e o FIN */
    CHECK(now_ms() - t0 < 1500);
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);         /* o peer recebeu "ultima" */
    close(sv[0]);

    CASE(91, "um peer que nao fecha: desiste ao fim do prazo");
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    t0 = now_ms();
    CHECK_INT(linger_drain(sv[0], 200), 1);
    long dt = now_ms() - t0;
    CHECK(dt >= 190 && dt < 1000);
    CHECK_INT(read(sv[1], buf, sizeof buf), 0);           /* mas o FIN saiu */
    close(sv[0]); close(sv[1]);
}

int main(void)
{
    caso_53_normal();
    caso_54_nao_le();
    caso_55_prazo_total();
    caso_56_peer_fechou();
    caso_57_meia_mensagem();
    caso_76_addr_to_str();
    caso_77_dual_stack();
    caso_90_wait_readable();
    caso_91_linger_drain();
    return t_report("net");
}
