#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int tcp_listen(const char *port, int backlog)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;      /* só IPv4, por agora */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags    = AI_PASSIVE;   /* host NULL => 0.0.0.0 (todas as interfaces) */

    int rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        /* Sem isto, reiniciar o servidor dá "Address already in use"
         * enquanto a ligação anterior está em TIME_WAIT. */
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(fd, backlog) == 0)
            break; /* sucesso */

        perror("bind/listen");
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
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

int send_all_timeout(int fd, const void *buf, size_t len, int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

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

ssize_t recv_retry(int fd, void *buf, size_t len)
{
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

void addr_to_str(const struct sockaddr *sa, char *out, size_t outlen)
{
    char ip[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;

    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)sa;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
        port = ntohs(a->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)sa;
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof ip);
        port = ntohs(a->sin6_port);
    }
    snprintf(out, outlen, "%s:%u", ip, port);
}
