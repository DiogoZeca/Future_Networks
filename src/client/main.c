/*
 * M1 — cliente de chat, em modo PEDIDO-RESPOSTA.
 *
 *   ligar -> NICK -> ler 1 resposta
 *   por cada linha do teclado: MSG -> ler 1 resposta -> mostrar
 *   Ctrl+D: half-close (shutdown SHUT_WR), mostrar o que faltar, sair
 *
 * Do lado do socket usa o MESMO framing que o servidor: as respostas também
 * podem chegar partidas ou coladas. Toda a conversão texto <-> mensagem está
 * em ui.c e protocol.c, testadas à unidade; aqui é só canalização.
 *
 * Limitação assumida do M1: só se lê o socket depois de enviar. Com o
 * servidor do M1 chega, porque cada pedido tem exatamente uma resposta. Se um
 * servidor não responder (D5/D6 ainda por acordar), o timeout evita ficar
 * preso para sempre. O M2 passa a ler o socket numa thread à parte.
 *
 * Uso: ./bin/client <host> [porta] [nick]   (sem nick, é pedido ao arrancar)
 */
#include "framing.h"
#include "net.h"
#include "protocol.h"
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PORT     "5000"
#define REPLY_TIMEOUT_S  5          /* quanto esperar por uma resposta */

/* Estado da ligação do lado do cliente: um rxbuf, como cada ligação no
 * servidor. static porque tem 8 KB e só há uma ligação. */
static struct rxbuf rx;

/* Lê UMA linha do servidor.
 *   1 = linha em line/len | 0 = timeout | -2 = o servidor fechou (FIN)
 *  -1 = erro ou linha demasiado longa
 * O fecho tem código próprio porque nem sempre é um problema: depois do
 * nosso Ctrl+D é exatamente o que esperamos. Quem chama decide se avisa.
 *
 * A ordem importa: primeiro ver se o rxbuf JÁ tem uma linha completa (o
 * servidor pode ter mandado duas respostas coladas num só segmento) e só
 * depois ir ao recv(). Ao contrário, a segunda resposta ficava esquecida no
 * buffer até chegarem mais bytes. */
static int read_line(int fd, char *line, size_t *len)
{
    char chunk[RX_CHUNK];

    for (;;) {
        int r = rxbuf_next_line(&rx, line, RX_LINE_BUF, len);
        if (r == RX_LINE)
            return 1;
        if (r != RX_NEED) {
            fprintf(stderr, "[client] o servidor mandou uma linha demasiado longa\n");
            return -1;
        }

        ssize_t n = recv_retry(fd, chunk, sizeof chunk);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;                       /* passou o SO_RCVTIMEO */
        if (n == 0)
            return -2;
        if (n < 0) {
            perror("recv");
            return -1;
        }
        if (rxbuf_feed(&rx, chunk, (size_t)n) != 0)
            return -1;                      /* inalcançável com recv <= RX_CHUNK */
    }
}

/* Mostra uma linha recebida. Devolve a mensagem em *m (type 0 se inválida). */
static void show_line(const char *line, size_t len, struct msg *m)
{
    struct proto_err err;
    char             text[PROTO_LINE_BUF];

    if (proto_parse(line, len, m, &err) != 0) {
        /* Ex.: um servidor que já tem tipos do M3 (EVENT, USERS...). Mostrar
         * em bruto em vez de rebentar ou esconder. */
        printf("[servidor mandou mensagem nao suportada (%s)] %.*s\n",
               err.reason, (int)len, line);
        return;
    }
    ui_format(m, text, sizeof text);
    printf("%s\n", text);
}

/* Codifica e envia. -1 = sair. */
static int send_msg(int fd, const struct msg *m)
{
    char   line[PROTO_LINE_BUF];
    size_t len;

    if (proto_encode(m, line, sizeof line, &len) != 0) {
        fprintf(stderr, "[client] BUG: mensagem nao codificavel\n");
        return -1;
    }
    if (send_all(fd, line, len) < 0) {
        perror("send");
        return -1;
    }
    return 0;
}

/* Espera pela resposta a um pedido e mostra-a (um OK só se show_ok, porque
 * o OK ao NICK é mostrado como "ligado como ..."). -1 = sair. */
static int await_reply(int fd, struct msg *reply, int show_ok)
{
    static char line[RX_LINE_BUF];
    size_t      len;
    struct proto_err err;

    memset(reply, 0, sizeof *reply);
    int r = read_line(fd, line, &len);
    if (r == 0) {
        printf("[sem resposta do servidor em %d s]\n", REPLY_TIMEOUT_S);
        return 0;
    }
    if (r == -2)
        fprintf(stderr, "[client] o servidor fechou a ligacao\n");
    if (r < 0)
        return -1;
    if (!show_ok && proto_parse(line, len, reply, &err) == 0 && reply->type == MT_OK)
        return 0;
    show_line(line, len, reply);
    return 0;
}

/* Lê o nick do teclado, se não veio na linha de comando. */
static int ask_nick(char *nick, size_t cap)
{
    char buf[128];
    printf("nick: ");
    fflush(stdout);
    if (fgets(buf, sizeof buf, stdin) == NULL)
        return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    snprintf(nick, cap, "%s", buf);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "uso: %s <host> [porta] [nick]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *host = argv[1];
    const char *port = (argc > 2) ? argv[2] : DEFAULT_PORT;

    setvbuf(stdout, NULL, _IOLBF, 0);       /* ver cada linha logo, mesmo em pipe */

    /* Validar o nick ANTES de ligar: com as mesmas regras do servidor, um
     * erro aqui poupa uma ida e volta pela rede só para ouvir um 400. */
    struct msg nick = { .type = MT_NICK };
    if (argc > 3)
        snprintf(nick.nick, sizeof nick.nick, "%s", argv[3]);
    else if (ask_nick(nick.nick, sizeof nick.nick) != 0)
        return EXIT_FAILURE;
    if (!proto_valid_nick(nick.nick)) {
        fprintf(stderr, "nick invalido: 1 a %d bytes, sem espacos nem "
                        "caracteres de controlo\n", PROTO_NICK_MAX);
        return EXIT_FAILURE;
    }

    int fd = tcp_connect(host, port);
    if (fd < 0)
        return EXIT_FAILURE;

    /* Timeout de leitura: recv() devolve -1 com EAGAIN se nada chegar a
     * tempo. É o que impede o pedido-resposta de ficar preso para sempre. */
    struct timeval tv = { .tv_sec = REPLY_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    rxbuf_init(&rx);
    struct msg reply;
    int status = EXIT_FAILURE;

    /* 1. NICK tem de ser a primeira mensagem (slide 124). */
    if (send_msg(fd, &nick) != 0 || await_reply(fd, &reply, 0) != 0)
        goto out;
    if (reply.type == MT_ERROR)             /* recusado: não vale a pena continuar */
        goto out;
    printf("[client] ligado a %s:%s como %s (Ctrl+D para sair)\n",
           host, port, nick.nick);

    /* 2. Uma linha do teclado -> um MSG -> uma resposta. */
    char typed[PROTO_TEXT_MAX + 3];         /* texto + "\r\n" + '\0' */
    while (fgets(typed, sizeof typed, stdin) != NULL) {
        /* Linha maior do que o buffer: o fgets parte-a, e o resto viria como
         * se fosse a linha seguinte — o mesmo problema do framing, agora do
         * lado do teclado. Deitar fora até ao '\n' e avisar. */
        if (strchr(typed, '\n') == NULL && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF)
                ;
            printf("[nao enviado] %s\n", UI_WHY_TOOLONG);
            continue;
        }

        struct msg  m;
        const char *why;
        int r = ui_input_to_msg(typed, &m, &why);
        if (r == 0)
            continue;                       /* Enter sozinho */
        if (r < 0) {
            printf("[nao enviado] %s\n", why);
            continue;
        }

        /* Sem eco local: o servidor devolve-nos o nosso CHAT (D5). Se o
         * imprimíssemos também, aparecia duplicado. */
        if (send_msg(fd, &m) != 0 || await_reply(fd, &reply, 1) != 0)
            goto out;
    }

    /* 3. EOF no teclado: fechar só o sentido de envio (FIN). O servidor vê
     * recv()==0 e fecha também; mostramos o que ainda chegar até lá. Aqui o
     * fecho do servidor (-2) é o fim esperado, por isso sem aviso. */
    shutdown(fd, SHUT_WR);
    for (;;) {
        static char line[RX_LINE_BUF];
        size_t      len;
        if (read_line(fd, line, &len) != 1)
            break;
        show_line(line, len, &reply);
    }
    status = EXIT_SUCCESS;

out:
    close(fd);
    return status;
}
