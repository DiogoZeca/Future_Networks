/* M5: as flags das interfaces (IFF_UP, IFF_BROADCAST... em <net/if.h>) e o
 * getifaddrs() não são POSIX: a glibc só as mostra com _DEFAULT_SOURCE. Tem
 * de vir ANTES de qualquer #include, e fica só neste ficheiro (o resto do
 * projeto continua em POSIX estrito, -D_POSIX_C_SOURCE=200809L). */
#define _DEFAULT_SOURCE

#include "net.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Tenta pôr um socket à escuta numa família (AF_INET6 ou AF_INET).
 * Devolve o fd, ou -1 com o motivo em *why, sem imprimir nada: falhar em
 * IPv6 numa máquina sem IPv6 é normal, quem chama é que decide se é erro. */
static int listen_family(int family, const char *port, int backlog, const char **why)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags    = AI_PASSIVE;   /* host NULL => "::" ou 0.0.0.0 (todas as interfaces) */

    int rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc != 0) {
        *why = gai_strerror(rc);      /* ex.: porta inválida */
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            *why = strerror(errno);   /* ex.: EAFNOSUPPORT sem IPv6 no kernel */
            continue;
        }

        /* Sem isto, reiniciar o servidor dá "Address already in use"
         * enquanto a ligação anterior está em TIME_WAIT (slide 125). */
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        /* Dual-stack: um socket IPv6 que também aceita IPv4 (os clientes
         * IPv4 aparecem como "::ffff:a.b.c.d"). EXPLÍCITO, porque o valor
         * por omissão varia: em Linux depende de /proc/sys/net/ipv6/
         * bindv6only (normalmente 0), em Windows é 1 (só IPv6). */
        if (rp->ai_family == AF_INET6) {
            int no = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof no);
        }

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(fd, backlog) == 0)
            break; /* sucesso */

        *why = strerror(errno);       /* ex.: EADDRINUSE */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int tcp_listen(const char *port, int backlog)
{
    /* Slide 85: "Use getaddrinfo() and sockaddr_storage for IPv4 / IPv6
     * portability". Primeiro IPv6 dual-stack (aceita as duas famílias num só
     * socket); se a máquina não tiver IPv6, IPv4 sozinho, como até ao M3. */
    const char *why6 = "?", *why4 = "?";
    int fd = listen_family(AF_INET6, port, backlog, &why6);
    if (fd >= 0)
        return fd;

    fd = listen_family(AF_INET, port, backlog, &why4);
    if (fd >= 0) {
        /* tcp_listen() é do servidor: vai para o log, como o resto (M4). */
        log_warn("sem IPv6 (%s): a escutar so em IPv4", why6);
        return fd;
    }
    log_error("listen na porta %s: %s", port, why4);
    return -1;
}

int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;    /* aceita IPv4 ou IPv6 */
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    /* Um nome pode resolver para vários endereços: tentar um a um. */
    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break; /* sucesso */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
        fprintf(stderr, "não foi possível ligar a %s:%s\n", host, port);
    return fd;
}

int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        /* MSG_NOSIGNAL: se o outro lado já fechou, send() devolve erro
         * (EPIPE) em vez de o processo morrer com SIGPIPE. */
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Milissegundos que faltam até `deadline` (CLOCK_MONOTONIC: não anda para
 * trás se alguém mudar a hora do sistema). <= 0 quer dizer "passou". */
static long ms_left(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(deadline->tv_sec - now.tv_sec) * 1000
         + (deadline->tv_nsec - now.tv_nsec) / 1000000;
}

/* deadline = agora + ms (CLOCK_MONOTONIC). */
static void deadline_in(struct timespec *d, long ms)
{
    clock_gettime(CLOCK_MONOTONIC, d);
    d->tv_sec  += ms / 1000;
    d->tv_nsec += (ms % 1000) * 1000000L;
    if (d->tv_nsec >= 1000000000L) {
        d->tv_sec++;
        d->tv_nsec -= 1000000000L;
    }
}

int send_all_timeout(int fd, const void *buf, size_t len, int timeout_ms)
{
    struct timespec deadline;
    deadline_in(&deadline, timeout_ms);

    const char *p = buf;
    while (len > 0) {
        /* MSG_DONTWAIT: este send() nunca bloqueia, mesmo num socket
         * bloqueante. Se o buffer de envio estiver cheio devolve EAGAIN, e
         * a espera é feita no poll(), que sabe quanto tempo falta. */
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            p   += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;                         /* o peer morreu */

        /* Buffer cheio: esperar que haja espaço, mas só até ao prazo. */
        long left = ms_left(&deadline);
        if (left <= 0)
            return SEND_TIMEOUT_EXPIRED;
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int r = poll(&pfd, 1, (int)left);
        if (r < 0 && errno != EINTR)
            return -1;
        if (r == 0)
            return SEND_TIMEOUT_EXPIRED;
        /* r > 0 (há espaço, ou erro/HUP que o próximo send() reporta) ou
         * EINTR: voltar ao send(). */
    }
    return 0;
}

int wait_readable(int fd, int stop_fd, long timeout_ms)
{
    struct timespec deadline;
    if (timeout_ms >= 0)
        deadline_in(&deadline, timeout_ms);

    struct pollfd pfd[2] = {
        { .fd = fd,      .events = POLLIN },
        { .fd = stop_fd, .events = POLLIN },  /* fd < 0: o poll() ignora-o */
    };
    for (;;) {
        long left = -1;
        if (timeout_ms >= 0) {
            left = ms_left(&deadline);
            if (left < 0)
                left = 0;
        }
        int r = poll(pfd, 2, (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;                      /* o prazo continua a contar */
            return -1;
        }
        if (r == 0)
            return NET_TIMEOUT;
        if (stop_fd >= 0 && pfd[1].revents != 0)
            return NET_STOP;
        if (pfd[0].revents != 0)               /* POLLIN, POLLHUP ou POLLERR */
            return NET_READY;
    }
}

int linger_drain(int fd, long timeout_ms)
{
    char sink[4096];

    shutdown(fd, SHUT_WR);                     /* FIN depois do que já saiu */
    struct timespec deadline;
    deadline_in(&deadline, timeout_ms);
    for (;;) {
        long left = ms_left(&deadline);
        if (left <= 0)
            return 1;
        int w = wait_readable(fd, -1, left);
        if (w == NET_TIMEOUT)
            return 1;
        if (w < 0)
            return -1;
        ssize_t n = recv_retry(fd, sink, sizeof sink);
        if (n == 0)
            return 0;                          /* o FIN do peer: limpo */
        if (n < 0)
            return -1;
        /* n > 0: bytes depois do fim (o resto da linha longa, o que veio
         * colado ao QUIT...). Deitar fora e continuar. */
    }
}

ssize_t recv_retry(int fd, void *buf, size_t len)
{
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

void addr_ip(const struct sockaddr *sa, char *out, size_t outlen)
{
    if (out == NULL || outlen == 0)
        return;
    const char *r = NULL;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
        r = inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)outlen);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))
            /* Um cliente IPv4 num socket dual-stack chega como
             * ::ffff:a.b.c.d. Mostra-se como o IPv4 que é. */
            r = inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], out, (socklen_t)outlen);
        else
            r = inet_ntop(AF_INET6, &a->sin6_addr, out, (socklen_t)outlen);
    }
    if (r == NULL)
        snprintf(out, outlen, "?");
}

void addr_to_str(const struct sockaddr *sa, char *out, size_t outlen)
{
    char     ip[INET6_ADDRSTRLEN];
    unsigned port = 0;
    int      v6   = 0;

    addr_ip(sa, ip, sizeof ip);
    if (sa->sa_family == AF_INET) {
        port = ntohs(((const struct sockaddr_in *)sa)->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        port = ntohs(a->sin6_port);
        v6   = !IN6_IS_ADDR_V4MAPPED(&a->sin6_addr);
    }
    /* IPv6 entre parênteses retos ("[::1]:5000"), como num URL: senão os
     * ':' do endereço confundem-se com o da porta. */
    snprintf(out, outlen, v6 ? "[%s]:%u" : "%s:%u", ip, port);
}

int sock_port(int fd)
{
    struct sockaddr_storage ss;
    socklen_t               len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0)
        return -1;
    if (ss.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    return -1;
}

/* ===== M5 — UDP ========================================================= */

int udp_bind(const char *port, const char **why)
{
    const char *dummy;
    if (why == NULL)
        why = &dummy;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;      /* só IPv4: broadcast (D32) */
    hints.ai_socktype = SOCK_DGRAM;   /* UDP */
    hints.ai_flags    = AI_PASSIVE;   /* host NULL => 0.0.0.0 */

    int rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc != 0) {
        *why = gai_strerror(rc);      /* ex.: porta inválida */
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        *why = strerror(errno);
    } else {
        int yes = 1;                  /* dois servidores na mesma máquina */
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
            *why = strerror(errno);   /* ex.: EADDRINUSE */
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    return fd;
}

int udp_broadcast_socket(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int udp_send(int fd, const void *buf, size_t len,
             const struct sockaddr *to, socklen_t tolen)
{
    for (;;) {
        ssize_t n = sendto(fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL, to, tolen);
        if (n >= 0)
            return (size_t)n == len ? 0 : -1; /* um datagrama é tudo ou nada */
        if (errno != EINTR)
            return -1;
    }
}

ssize_t udp_recv(int fd, void *buf, size_t cap, struct sockaddr_storage *from)
{
    for (;;) {
        /* recvmsg e não recvfrom: é o msg_flags que diz, de forma portável,
         * se o datagrama não cabia (MSG_TRUNC). O recvfrom devolvia só os
         * cap primeiros bytes como se fosse o datagrama todo. */
        struct iovec  iov = { .iov_base = buf, .iov_len = cap };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_name    = from;
        mh.msg_namelen = from != NULL ? sizeof *from : 0;
        mh.msg_iov     = &iov;
        mh.msg_iovlen  = 1;

        ssize_t n = recvmsg(fd, &mh, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (mh.msg_flags & MSG_TRUNC)
            return NET_TRUNCATED;
        return n;
    }
}

/* Acrescenta o endereço `a` (com a porta) a out[], se ainda lá não estiver e
 * houver lugar. Devolve o novo n. */
static size_t add_target(struct sockaddr_in *out, size_t n, size_t max,
                         struct in_addr a, int port)
{
    for (size_t i = 0; i < n; i++)
        if (out[i].sin_addr.s_addr == a.s_addr)
            return n;
    if (n >= max)
        return n;
    memset(&out[n], 0, sizeof out[n]);
    out[n].sin_family = AF_INET;
    out[n].sin_port   = htons((uint16_t)port);
    out[n].sin_addr   = a;
    return n + 1;
}

size_t broadcast_targets(int port, struct sockaddr_in *out, size_t max)
{
    if (port < 1 || port > 65535 || out == NULL || max == 0)
        return 0;

    struct in_addr limited = { .s_addr = htonl(INADDR_BROADCAST) };
    size_t         n       = add_target(out, 0, max, limited, port);

    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) != 0)
        return n;                     /* fica o limitado, que é melhor do que nada */
    for (struct ifaddrs *i = ifs; i != NULL; i = i->ifa_next) {
        unsigned f = i->ifa_flags;
        if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET ||
            !(f & IFF_UP) || !(f & IFF_BROADCAST) || (f & IFF_LOOPBACK) ||
            i->ifa_broadaddr == NULL)
            continue;
        const struct sockaddr_in *b = (const struct sockaddr_in *)i->ifa_broadaddr;
        n = add_target(out, n, max, b->sin_addr, port);
    }
    freeifaddrs(ifs);
    return n;
}
