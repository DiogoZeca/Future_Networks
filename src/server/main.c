/*
 * M1 — servidor de chat: mensagens JSON delimitadas por '\n'.
 *
 * Por cada ligação:
 *
 *   recv() --bytes--> rxbuf (framing) --linhas--> session (protocol + estado)
 *                                                     |
 *   send_all() <--linha JSON-- proto_encode() <-- resposta
 *
 * Toda a lógica está nas camadas testadas à unidade; este ficheiro é só a
 * "canalização" entre elas e os sockets.
 *
 * Continua ITERATIVO, como no M0: atende UM cliente de cada vez. Um segundo
 * cliente liga-se (o kernel faz o handshake) mas só é atendido quando o
 * primeiro sair. O M2 resolve isto.
 *
 * Uso: ./bin/server [porta]
 */
#include "framing.h"
#include "net.h"
#include "protocol.h"
#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_PORT "5000"

/* Codifica e envia uma resposta. -1 = fechar a ligação. */
static int send_reply(int fd, const char *who, const struct msg *r)
{
    char   line[PROTO_LINE_BUF];
    size_t len;

    if (proto_encode(r, line, sizeof line, &len) != 0) {
        /* Nunca deve acontecer: a session só produz respostas válidas (os
         * testes verificam-no). Se acontecer, é um bug NOSSO. */
        fprintf(stderr, "[server] %s: BUG: resposta %s nao codificavel\n",
                who, proto_type_name(r->type));
        return -1;
    }
    if (r->type == MT_ERROR)
        printf("[server] %s -> ERROR %d (%s)\n", who, r->code, r->reason);
    else
        printf("[server] %s -> %s\n", who, proto_type_name(r->type));

    if (send_all(fd, line, len) < 0) {
        perror("send");
        return -1;
    }
    return 0;
}

/* Serve uma ligação até ela fechar. */
static void serve(int fd, const char *who)
{
    /* static: o rxbuf tem 8 KB e só há uma ligação de cada vez. No M2, com
     * uma thread por cliente, passa a viver dentro de uma struct por ligação. */
    static struct rxbuf rx;
    static char         chunk[RX_CHUNK];
    static char         line[RX_LINE_BUF];
    struct session      sess;
    struct msg          reply;

    rxbuf_init(&rx);
    session_init(&sess);

    for (;;) {
        /* Nunca mais do que RX_CHUNK por recv(): é essa a disciplina que
         * garante que o rxbuf não transborda (framing.md §3.1). */
        ssize_t n = recv_retry(fd, chunk, sizeof chunk);
        if (n == 0) {
            printf("[server] %s fechou a ligacao\n", who);
            return;
        }
        if (n < 0) {
            perror("recv");
            return;
        }

        if (rxbuf_feed(&rx, chunk, (size_t)n) != 0) {
            /* Inalcançável com recv de <= RX_CHUNK e o buffer sempre drenado. */
            fprintf(stderr, "[server] %s: BUG: rxbuf_feed falhou\n", who);
            return;
        }

        /* Um recv() pode trazer zero, uma ou VÁRIAS mensagens: ciclo, não if.
         * E três saídas, não duas (framing.h). */
        for (;;) {
            size_t len;
            int r = rxbuf_next_line(&rx, line, sizeof line, &len);

            if (r == RX_NEED)                   /* faltam bytes: voltar ao recv() */
                break;

            if (r == RX_TOOLONG) {              /* culpa DELES (D1/D2) */
                struct msg e = { .type = MT_ERROR, .code = 400,
                                 .reason = SESS_R_TOOLONG };
                send_reply(fd, who, &e);
                /* Fechar sem ressincronizar (D2). Atenção: se ainda houver
                 * bytes deles por ler, o close() manda RST em vez de FIN
                 * (M0.md, nota 4) e o cliente pode nem chegar a ler o ERROR.
                 * Resolver isso a sério precisa de timeouts — M4. */
                return;
            }

            if (r != RX_LINE) {                 /* RX_FAULT: culpa NOSSA */
                fprintf(stderr, "[server] %s: BUG: rxbuf_next_line = %d\n", who, r);
                return;
            }

            session_handle(&sess, line, len, &reply);
            if (send_reply(fd, who, &reply) != 0)
                return;
        }
    }
}

int main(int argc, char *argv[])
{
    const char *port = (argc > 1) ? argv[1] : DEFAULT_PORT;

    /* Mostrar cada linha de log logo, mesmo com o stdout redirecionado
     * para um ficheiro ou pipe (por omissão aí só sai em blocos de 4 KB). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int listener = tcp_listen(port, 8);
    if (listener < 0)
        return EXIT_FAILURE;
    printf("[server] a escuta na porta %s\n", port);

    for (;;) {
        struct sockaddr_storage peer;          /* cabe IPv4 ou IPv6 */
        socklen_t peer_len = sizeof peer;

        /* accept() devolve um socket NOVO, só para este cliente.
         * O listener continua à escuta. */
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        char who[64];
        addr_to_str((struct sockaddr *)&peer, who, sizeof who);
        printf("[server] ligado: %s\n", who);

        serve(fd, who);

        close(fd);
        printf("[server] desligado: %s\n", who);
    }

    close(listener);
    return EXIT_SUCCESS;
}
