/*
 * discover.c — M5: procurar servidores na LAN e escolher um.
 * Contrato e porquês em include/discover.h.
 */
#include "discover.h"
#include "net.h"
#include "ui.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int to_addr(const char *ip, int port, struct sockaddr_in *out)
{
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port   = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &out->sin_addr) == 1 ? 0 : -1;
}

/* Manda o pedido a todos os destinos. Devolve quantos envios correram bem
 * (um destino pode falhar — uma interface sem rota — e os outros não). */
static size_t send_round(int fd, const struct sockaddr_in *to, size_t n)
{
    size_t ok = 0;
    for (size_t i = 0; i < n; i++)
        if (udp_send(fd, DISCO_REQUEST, strlen(DISCO_REQUEST),
                     (const struct sockaddr *)&to[i], sizeof to[i]) == 0)
            ok++;
    return ok;
}

/* Lê todos os datagramas que já chegaram. Os que não são uma resposta (lixo,
 * grandes de mais, outro tipo) ficam de fora sem mais. */
static void drain(int fd, struct disco_list *out, struct discover_info *info)
{
    for (;;) {
        char                    buf[DISCO_DGRAM_MAX];
        struct sockaddr_storage from;
        ssize_t n = udp_recv(fd, buf, sizeof buf, &from);
        if (n == -1)
            return;                             /* EAGAIN: acabaram-se */
        struct disco_server s;
        if (n < 0 || disco_parse_reply(buf, (size_t)n, &s) != 0)
            continue;
        /* O IP é o de ORIGEM do datagrama: chegou até nós, logo é alcançável
         * (typical-problems §4.4). */
        addr_ip((struct sockaddr *)&from, s.ip, sizeof s.ip);
        if (disco_list_add(out, &s) < 0)
            info->dropped++;
    }
}

int discover(const struct discover_opts *o, struct disco_list *out,
             struct discover_info *info)
{
    disco_list_init(out);
    memset(info, 0, sizeof *info);

    struct sockaddr_in to[DISCOVER_DEST_MAX];
    size_t             nto = 0;
    if (o->n_targets > 0) {
        for (size_t i = 0; i < o->n_targets && nto < DISCOVER_DEST_MAX; i++) {
            if (to_addr(o->targets[i], o->udp_port, &to[nto]) != 0) {
                errno = EINVAL;
                return -1;
            }
            nto++;
        }
    } else {
        nto = broadcast_targets(o->udp_port, to, DISCOVER_DEST_MAX);
    }
    if (nto == 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = udp_broadcast_socket();
    if (fd < 0)
        return -1;

    long start = now_ms();
    long end   = start + o->wait_ms;
    long gap   = o->wait_ms / 5;                /* 1500 ms -> envios a 0, 300, 600 */
    long next  = start;
    int  tries = 0;

    for (;;) {
        long t = now_ms();
        if (tries < DISCOVER_TRIES && t >= next) {
            size_t ok = send_round(fd, to, nto);
            if (ok == 0 && tries == 0 && o->n_targets == 0) {
                /* Sem rede: procurar só nesta máquina (discover.h). */
                to_addr(DISCOVER_LOOPBACK, o->udp_port, &to[0]);
                nto                 = 1;
                info->loopback_only = 1;
                ok                  = send_round(fd, to, nto);
            }
            if (ok == 0 && tries == 0) {        /* nem isso: desistir já */
                int e = errno;
                close(fd);
                errno = e;
                return -1;
            }
            tries++;
            next = t + gap;
        }
        if (t >= end)
            break;

        long until = end;
        if (tries < DISCOVER_TRIES && next < until)
            until = next;
        int w = wait_readable(fd, -1, until - t > 0 ? until - t : 0);
        if (w == NET_READY)
            drain(fd, out, info);
        else if (w < 0)
            break;
    }
    drain(fd, out, info);                       /* o que chegou mesmo no fim */
    info->n_dest = nto;
    close(fd);
    return 0;
}

int discover_choose(const struct disco_list *l)
{
    char line[DISCO_NAME_MAX + DISCO_IP_BUF + 64];

    if (l->n == 0)
        return -1;
    if (l->n == 1) {
        disco_format(&l->s[0], 1, line, sizeof line);
        printf("[client] encontrado: %s\n", line);
        return 0;
    }

    printf("[client] %zu servidores encontrados:\n", l->n);
    for (size_t i = 0; i < l->n; i++) {
        disco_format(&l->s[i], i + 1, line, sizeof line);
        printf("  %s\n", line);
    }
    for (;;) {
        printf("escolhe um servidor [1-%zu]: ", l->n);
        fflush(stdout);
        char typed[32];
        if (fgets(typed, sizeof typed, stdin) == NULL) {
            printf("\n");                     /* o prompt não fica pendurado */
            return -1;
        }
        int k = ui_parse_choice(typed, l->n);
        if (k > 0)
            return k - 1;
        if (strchr(typed, '\n') == NULL) {      /* linha comprida: deitar fora o resto */
            int c;
            while ((c = getchar()) != EOF && c != '\n')
                ;
        }
        printf("[client] escreve um numero de 1 a %zu\n", l->n);
    }
}
