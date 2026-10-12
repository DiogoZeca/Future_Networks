/*
 * M0 — servidor de eco TCP.
 *
 * Iterativo: atende UM cliente de cada vez. Um segundo cliente consegue
 * ligar-se (o kernel completa o handshake e põe-no na fila do listen),
 * mas só é atendido quando o primeiro sair. O M2 resolve isto.
 *
 * Uso: ./bin/server [porta]
 */
#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_PORT "5000"
#define BUF_SIZE     4096

int main(int argc, char *argv[])
{
    const char *port = (argc > 1) ? argv[1] : DEFAULT_PORT;

    /* Mostrar cada linha de log logo, mesmo com o stdout redirecionado
     * para um ficheiro ou pipe (por omissão aí só sai em blocos de 4 KB). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int listener = tcp_listen(port, 8);
    if (listener < 0)
        return EXIT_FAILURE;
    printf("[server] à escuta na porta %s\n", port);

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

        char buf[BUF_SIZE];
        size_t total = 0;
        for (;;) {
            ssize_t n = recv_retry(fd, buf, sizeof buf);
            if (n == 0)                        /* cliente fechou (FIN) */
                break;
            if (n < 0) {
                perror("recv");
                break;
            }
            /* Repara: um recv() não corresponde a uma "mensagem".
             * Pode trazer meia linha, ou várias juntas. */
            printf("[server] %s: recv de %zd bytes\n", who, n);

            if (send_all(fd, buf, (size_t)n) < 0) {
                perror("send");
                break;
            }
            total += (size_t)n;
        }

        printf("[server] desligado: %s (%zu bytes ecoados)\n", who, total);
        close(fd);
    }

    close(listener);
    return EXIT_SUCCESS;
}
