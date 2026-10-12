/*
 * M0 — cliente de eco TCP.
 *
 * Lê linhas do teclado, envia-as e espera pelo eco completo.
 * Ctrl+D (EOF) termina com half-close (shutdown SHUT_WR), como no slide 87.
 *
 * Uso: ./bin/client <host> [porta]
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT "5000"
#define BUF_SIZE     4096

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "uso: %s <host> [porta]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *host = argv[1];
    const char *port = (argc > 2) ? argv[2] : DEFAULT_PORT;

    int fd = tcp_connect(host, port);
    if (fd < 0)
        return EXIT_FAILURE;
    printf("[client] ligado a %s:%s (Ctrl+D para sair)\n", host, port);

    char line[BUF_SIZE];
    char echo[BUF_SIZE];

    while (fgets(line, sizeof line, stdin) != NULL) {
        size_t len = strlen(line);

        if (send_all(fd, line, len) < 0) {
            perror("send");
            goto out;
        }

        /* O eco pode chegar partido em vários recv():
         * continuar a ler até ter recebido os len bytes de volta. */
        size_t got = 0;
        int recvs = 0;
        while (got < len) {
            ssize_t n = recv_retry(fd, echo + got, len - got);
            if (n == 0) {
                fprintf(stderr, "[client] o servidor fechou a ligação\n");
                goto out;
            }
            if (n < 0) {
                perror("recv");
                goto out;
            }
            got += (size_t)n;
            recvs++;
        }
        printf("[eco em %d recv] %.*s", recvs, (int)got, echo);
    }

    /* EOF no teclado: fechamos só o sentido de envio (FIN para o servidor).
     * O servidor vê recv()==0, fecha também, e nós vemos recv()==0. */
    shutdown(fd, SHUT_WR);
    while (recv_retry(fd, echo, sizeof echo) > 0)
        ;

out:
    close(fd);
    return EXIT_SUCCESS;
}
