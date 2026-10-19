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
 *
 * M5 (UDP, para a descoberta — disco.h). Com sockets UDP reais, mas só na
 * loopback: 127.0.0.1 e o broadcast da loopback, 127.255.255.255, que é um
 * broadcast a sério (só entre os programas desta máquina) e funciona mesmo
 * numa máquina sem rede nenhuma:
 *  100  addr_ip(): só o IP, sem porta nem parênteses
 *  101  udp_bind(): porta escolhida pelo kernel, sock_port(), dois servidores
 *       na mesma porta (SO_REUSEADDR), porta tomada por outro programa
 *  102  udp_send()/udp_recv(): o IP de origem, responder-lhe, nunca
 *       bloquear, e um datagrama grande de mais deitado fora INTEIRO
 *  103  broadcast: sem SO_BROADCAST o kernel recusa (EACCES); com ele, o
 *       datagrama chega aos DOIS servidores da mesma porta — um unicast só
 *       a um
 *  104  broadcast_targets(): 255.255.255.255 primeiro, sem repetidos, sem a
 *       loopback, e os limites
 */
#include "disco.h"
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

/* ===== M5 — UDP ========================================================= */

/* Um sockaddr_in para ip:port (ip em texto). */
static struct sockaddr_in v4(const char *ip, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

/* Espera até 500 ms por um datagrama em fd. Devolve o que o udp_recv() deu
 * (ou -1 se nada chegou). */
static ssize_t recv_wait(int fd, char *buf, size_t cap, struct sockaddr_storage *from)
{
    if (wait_readable(fd, -1, 500) != NET_READY)
        return -1;
    return udp_recv(fd, buf, cap, from);
}

static void caso_100_addr_ip(void)
{
    char ip[64];

    CASE(100, "IPv4: so o IP");
    struct sockaddr_in a = v4("192.168.1.10", 5000);
    addr_ip((struct sockaddr *)&a, ip, sizeof ip);
    CHECK_STR(ip, "192.168.1.10");

    CASE(100, "IPv6 sem parenteses; ::ffff:a.b.c.d como IPv4");
    struct sockaddr_in6 b;
    memset(&b, 0, sizeof b);
    b.sin6_family = AF_INET6;
    b.sin6_port   = htons(5000);
    inet_pton(AF_INET6, "fe80::1", &b.sin6_addr);
    addr_ip((struct sockaddr *)&b, ip, sizeof ip);
    CHECK_STR(ip, "fe80::1");
    inet_pton(AF_INET6, "::ffff:10.0.0.7", &b.sin6_addr);
    addr_ip((struct sockaddr *)&b, ip, sizeof ip);
    CHECK_STR(ip, "10.0.0.7");

    CASE(100, "familia que nao e IP: \"?\"; e o addr_to_str continua igual");
    struct sockaddr u;
    memset(&u, 0, sizeof u);
    u.sa_family = AF_UNIX;
    addr_ip(&u, ip, sizeof ip);
    CHECK_STR(ip, "?");
    addr_to_str((struct sockaddr *)&a, ip, sizeof ip);
    CHECK_STR(ip, "192.168.1.10:5000");
}

static void caso_101_udp_bind(void)
{
    const char *why = NULL;

    CASE(101, "porta \"0\": o kernel escolhe, e o sock_port() diz qual");
    int a = udp_bind("0", &why);
    CHECK(a >= 0);
    int port = sock_port(a);
    CHECK(port > 0 && port <= 65535);

    CASE(101, "um 2.o servidor na MESMA porta (SO_REUSEADDR)");
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    int b = udp_bind(ps, &why);
    CHECK(b >= 0);
    CHECK_INT(sock_port(b), port);

    CASE(101, "porta tomada por um programa SEM SO_REUSEADDR: -1 e o motivo");
    int other = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in any = v4("0.0.0.0", 0);
    CHECK_INT(bind(other, (struct sockaddr *)&any, sizeof any), 0);
    snprintf(ps, sizeof ps, "%d", sock_port(other));
    why = NULL;
    CHECK_INT(udp_bind(ps, &why), -1);
    CHECK(why != NULL && strstr(why, "in use") != NULL);

    CASE(101, "porta invalida: -1 e o motivo; why pode ser NULL");
    why = NULL;
    CHECK_INT(udp_bind("nao-e-porta", &why), -1);
    CHECK(why != NULL);
    CHECK_INT(udp_bind("nao-e-porta", NULL), -1);

    CASE(101, "sock_port() de um fd que nao e socket: -1");
    CHECK_INT(sock_port(-1), -1);

    close(a); close(b); close(other);
}

static void caso_102_send_recv(void)
{
    char                    buf[DISCO_DGRAM_MAX];
    struct sockaddr_storage from;
    char                    ip[64];

    int srv = udp_bind("0", NULL);
    int cli = udp_broadcast_socket();
    CHECK(srv >= 0 && cli >= 0);
    struct sockaddr_in to = v4("127.0.0.1", sock_port(srv));

    CASE(102, "nada para ler: -1/EAGAIN logo, sem bloquear");
    long t0 = now_ms();
    errno = 0;
    CHECK_INT(udp_recv(srv, buf, sizeof buf, &from), -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    CHECK(now_ms() - t0 < 100);

    CASE(102, "um pedido chega inteiro, com o IP e a porta de quem o mandou");
    CHECK_INT(udp_send(cli, DISCO_REQUEST, strlen(DISCO_REQUEST),
                       (struct sockaddr *)&to, sizeof to), 0);
    ssize_t n = recv_wait(srv, buf, sizeof buf, &from);
    CHECK_INT(n, strlen(DISCO_REQUEST));
    CHECK(n > 0 && memcmp(buf, DISCO_REQUEST, (size_t)n) == 0);
    addr_ip((struct sockaddr *)&from, ip, sizeof ip);
    CHECK_STR(ip, "127.0.0.1");
    CHECK_INT(ntohs(((struct sockaddr_in *)&from)->sin_port), sock_port(cli));

    CASE(102, "a resposta para esse endereco chega ao cliente, vinda do servidor");
    CHECK_INT(udp_send(srv, "{\"port\":5000}", 13, (struct sockaddr *)&from, sizeof(struct sockaddr_in)), 0);
    n = recv_wait(cli, buf, sizeof buf, &from);
    CHECK_INT(n, 13);
    CHECK_INT(ntohs(((struct sockaddr_in *)&from)->sin_port), sock_port(srv));

    CASE(102, "datagrama vazio: 0 bytes (um datagrama, nao um fecho)");
    CHECK_INT(udp_send(cli, "", 0, (struct sockaddr *)&to, sizeof to), 0);
    CHECK_INT(recv_wait(srv, buf, sizeof buf, NULL), 0);

    CASE(102, "maior do que o buffer: NET_TRUNCATED, e deitado fora INTEIRO");
    char big[DISCO_DGRAM_MAX + 100];
    memset(big, 'x', sizeof big);
    CHECK_INT(udp_send(cli, big, sizeof big, (struct sockaddr *)&to, sizeof to), 0);
    CHECK_INT(udp_send(cli, "seguinte", 8, (struct sockaddr *)&to, sizeof to), 0);
    CHECK_INT(recv_wait(srv, buf, sizeof buf, &from), NET_TRUNCATED);
    n = recv_wait(srv, buf, sizeof buf, &from);   /* o resto NAO aparece aqui */
    CHECK_INT(n, 8);
    CHECK(n == 8 && memcmp(buf, "seguinte", 8) == 0);

    close(srv); close(cli);
}

static void caso_103_broadcast(void)
{
    char buf[64];

    int a = udp_bind("0", NULL);
    char ps[16];
    snprintf(ps, sizeof ps, "%d", sock_port(a));
    int b = udp_bind(ps, NULL);                    /* 2 servidores, mesma porta */
    struct sockaddr_in bc = v4("127.255.255.255", sock_port(a));
    struct sockaddr_in uc = v4("127.0.0.1", sock_port(a));

    CASE(103, "sem SO_BROADCAST o kernel recusa enviar para um broadcast (EACCES)");
    int plain = socket(AF_INET, SOCK_DGRAM, 0);
    errno = 0;
    CHECK_INT(udp_send(plain, "x", 1, (struct sockaddr *)&bc, sizeof bc), -1);
    CHECK_INT(errno, EACCES);

    CASE(103, "udp_broadcast_socket() liga o SO_BROADCAST");
    int cli = udp_broadcast_socket();
    int on = 0;
    socklen_t len = sizeof on;
    CHECK_INT(getsockopt(cli, SOL_SOCKET, SO_BROADCAST, &on, &len), 0);
    CHECK(on != 0);

    CASE(103, "um broadcast chega aos DOIS servidores da mesma porta");
    CHECK_INT(udp_send(cli, "bc", 2, (struct sockaddr *)&bc, sizeof bc), 0);
    CHECK_INT(recv_wait(a, buf, sizeof buf, NULL), 2);
    CHECK_INT(recv_wait(b, buf, sizeof buf, NULL), 2);

    CASE(103, "um unicast para o mesmo IP e porta chega so a UM deles");
    CHECK_INT(udp_send(cli, "uc", 2, (struct sockaddr *)&uc, sizeof uc), 0);
    int got = (recv_wait(a, buf, sizeof buf, NULL) == 2) + (recv_wait(b, buf, sizeof buf, NULL) == 2);
    CHECK_INT(got, 1);

    close(a); close(b); close(plain); close(cli);
}

static void caso_104_targets(void)
{
    struct sockaddr_in t[16];
    char               ip[64];

    CASE(104, "o 1.o e sempre 255.255.255.255, com a porta");
    size_t n = broadcast_targets(5001, t, 16);
    CHECK(n >= 1);
    addr_ip((struct sockaddr *)&t[0], ip, sizeof ip);
    CHECK_STR(ip, "255.255.255.255");

    CASE(104, "todos IPv4, com a porta, sem repetidos e sem a loopback");
    printf("  (destinos nesta maquina:");
    for (size_t i = 0; i < n; i++) {
        addr_ip((struct sockaddr *)&t[i], ip, sizeof ip);
        printf(" %s", ip);
        CHECK_INT(t[i].sin_family, AF_INET);
        CHECK_INT(ntohs(t[i].sin_port), 5001);
        CHECK((ntohl(t[i].sin_addr.s_addr) >> 24) != 127);
        for (size_t j = 0; j < i; j++)
            CHECK(t[j].sin_addr.s_addr != t[i].sin_addr.s_addr);
    }
    printf(")\n");

    CASE(104, "limites: max 1 so o limitado; max 0, porta 0 ou 70000: nada");
    CHECK_INT(broadcast_targets(5001, t, 1), 1);
    CHECK_INT(broadcast_targets(5001, t, 0), 0);
    CHECK_INT(broadcast_targets(0, t, 16), 0);
    CHECK_INT(broadcast_targets(70000, t, 16), 0);
    CHECK_INT(broadcast_targets(5001, NULL, 16), 0);
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
    caso_100_addr_ip();
    caso_101_udp_bind();
    caso_102_send_recv();
    caso_103_broadcast();
    caso_104_targets();
    return t_report("net");
}
