/*
 * M3 — cliente de chat, com DUAS threads e os comandos do slide 123.
 *
 *   thread principal   teclado -> MSG / PRIV / LIST / QUIT -> socket
 *   thread de leitura  socket  -> mostrar no ecrã
 *
 * Do lado do socket usa o MESMO framing que o servidor: as respostas também
 * podem chegar partidas ou coladas. Toda a conversão texto <-> mensagem está
 * em ui.c e protocol.c, testadas à unidade; aqui é só canalização.
 *
 *
 * PORQUE É QUE O CLIENTE DO M1 DEIXOU DE CHEGAR
 * ---------------------------------------------
 * No M1 era pedido-resposta: enviar, ler UMA resposta, repetir. Funcionava
 * porque o servidor respondia exatamente uma linha a cada linha recebida, e só
 * a quem a enviou.
 *
 * No M2 isso deixa de ser verdade de duas maneiras:
 *
 *   - chegam mensagens que não pedimos (o CHAT de outra pessoa, um EVENT de
 *     entrada ou saída), a qualquer momento;
 *   - um envio nosso pode ter MAIS do que uma resposta à frente na fila.
 *
 * Com o ciclo do M1, as mensagens dos outros ficavam no buffer do kernel e só
 * apareciam quando NÓS escrevêssemos alguma coisa — e desalinhadas, porque o
 * await_reply lia só uma linha de cada vez. Um chat em que só se vê o que os
 * outros dizem depois de falarmos não é um chat.
 *
 *
 * PORQUE É QUE NÃO HÁ LOCKS NENHUNS AQUI
 * --------------------------------------
 * Uma ligação TCP é full-duplex: os dois sentidos são independentes. Como cada
 * thread usa um sentido só — a principal escreve, a de leitura lê — nenhuma
 * estrutura é partilhada:
 *
 *   rx (rxbuf)   só a thread de leitura lhe toca, depois do arranque
 *   send_all()   só a thread principal o chama
 *   printf()     cada chamada é atómica na glibc; as linhas podem aparecer
 *                por ordens diferentes, mas nunca partidas a meio
 *
 * É por isso que o servidor precisa de um wlock por cliente e o cliente não
 * precisa de nenhum: lá há N threads a escrever no mesmo socket, aqui há uma.
 *
 *
 * O QUE MUDOU DO M2 PARA O M3
 * ---------------------------
 *   - Comandos: /list, /msg <nick> <texto>, /quit, /help (ui.c, testados à
 *     unidade). Um comando desconhecido NÃO é enviado como texto: "/lsit"
 *     ir para o chat inteiro por um erro de escrita era o pior resultado.
 *   - Nick em uso (ERROR 409): em vez de sair, pede outro e tenta na mesma
 *     ligação (D19) — o servidor deixa.
 *   - Sair (/quit ou Ctrl+D) manda um QUIT antes de fechar. Contra um
 *     servidor que não feche ao receber o QUIT, o shutdown(SHUT_WR) a seguir
 *     garante na mesma o fim da ligação.
 *
 * Uso: ./bin/client <host> [porta] [nick]   (sem nick, é pedido ao arrancar)
 */
#include "framing.h"
#include "net.h"
#include "protocol.h"
#include "ui.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PORT     "5000"
#define REPLY_TIMEOUT_S  5          /* só para a resposta ao NICK */

/* Estado da ligação: um rxbuf, como cada ligação no servidor. */
static struct rxbuf rx;

/* 1 = fomos nós que decidimos sair (Ctrl+D). Serve para a thread de leitura
 * saber se o fecho do servidor é o fim esperado ou uma queda.
 *
 * _Atomic e não `volatile sig_atomic_t`: esse é o idioma para SINAIS, e não dá
 * garantia nenhuma de sincronização entre THREADS — o ThreadSanitizer aponta-o
 * como data race, e tem razão. Com _Atomic (C11) a leitura e a escrita são
 * atómicas e sequencialmente consistentes, sem precisar de um mutex para um
 * único inteiro. */
static _Atomic int quitting = 0;

/* Lê UMA linha do servidor.
 *   1 = linha em line/len | 0 = timeout | -2 = o servidor fechou (FIN)
 *  -1 = erro ou linha demasiado longa
 *
 * A ordem importa: primeiro ver se o rxbuf JÁ tem uma linha completa (o
 * servidor pode ter mandado várias coladas num só segmento) e só depois ir ao
 * recv(). Ao contrário, a segunda ficava esquecida no buffer até chegarem mais
 * bytes — e com um CHAT à frente de um EVENT isso dava-se sempre. */
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
            if (!quitting)
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
        /* Ex.: um servidor de outro grupo com uma extensão que não
         * conhecemos. Mostrar em bruto em vez de rebentar ou esconder. */
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

/* ---- Thread de leitura -------------------------------------------------- */

/* Lê o socket até ao fim e mostra tudo o que chegar. Nunca escreve no socket.
 *
 * Se o servidor cair enquanto estamos à espera do teclado, a thread principal
 * está presa no fgets() e não há forma portável de a interromper — por isso
 * esta thread avisa e termina o processo. Quando somos NÓS a sair (Ctrl+D), o
 * fecho é esperado: basta retornar e deixar o join acontecer. */
static void *reader_thread(void *arg)
{
    int         fd = *(int *)arg;
    static char line[RX_LINE_BUF];           /* 4 KB: fora da stack da thread */
    size_t      len;
    struct msg  m;

    for (;;) {
        int r = read_line(fd, line, &len);
        if (r == 1) {
            show_line(line, len, &m);
            continue;
        }
        if (r == 0)
            continue;                        /* timeout: não é fim de nada */
        if (quitting)
            return NULL;                     /* fim esperado: o join trata */

        if (r == -2)
            fprintf(stderr, "\n[client] o servidor fechou a ligacao\n");
        exit(r == -2 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
}

/* ---- Arranque ----------------------------------------------------------- */

/* Lê um nick do teclado (se não veio na linha de comando, ou depois de um
 * 409) até ser válido pelas regras do protocolo. -1 = EOF. */
static int ask_nick(char *nick, size_t cap)
{
    char buf[128];
    for (;;) {
        printf("nick: ");
        fflush(stdout);
        if (fgets(buf, sizeof buf, stdin) == NULL)
            return -1;
        buf[strcspn(buf, "\r\n")] = '\0';
        if (proto_valid_nick(buf)) {
            snprintf(nick, cap, "%s", buf);
            return 0;
        }
        printf("[nao enviado] %s\n", UI_WHY_BADNICK);
    }
}

/* Envia o NICK e espera a resposta, AINDA em pedido-resposta.
 *
 * Esta fase fica síncrona de propósito: só vale a pena arrancar o chat depois
 * de sermos aceites, e enquanto ela dura mais ninguém toca no rxbuf.
 *
 * O NOSSO servidor garante que a primeira linha é a resposta ao NICK
 * (PROTOCOL.md §4). Mas não se assume isso: só um OK é aceitação e só um
 * ERROR é recusa. Tudo o resto (um EVENT, umas boas-vindas de um servidor de
 * outro grupo) é mostrado e continua-se à espera. Antes, qualquer coisa que
 * não fosse ERROR contava como OK, e essa linha desaparecia sem ser mostrada.
 * Cada leitura tem o timeout de REPLY_TIMEOUT_S.
 *
 * M3: um ERROR 409 (nick em uso) não é o fim — pede-se outro nick ao
 * utilizador e volta-se a tentar na mesma ligação (D19). Qualquer outro
 * ERROR continua a ser recusa.
 *
 * Devolve 0 se fomos aceites (com o nick final em *nick). */
static int do_nick(int fd, struct msg *nick)
{
    static char line[RX_LINE_BUF];
    size_t      len;
    struct msg  reply;

again:
    if (send_msg(fd, nick) != 0)
        return -1;

    /* Esperar pela resposta ao NICK: OK ou ERROR. Um servidor de outro grupo
     * pode mandar outras coisas antes (um EVENT, uma mensagem de boas-vindas):
     * mostrá-las e continuar à espera, em vez de as tomar por um OK. */
    for (;;) {
        int r = read_line(fd, line, &len);
        if (r == 0) {
            fprintf(stderr, "[client] sem resposta do servidor em %d s\n", REPLY_TIMEOUT_S);
            return -1;
        }
        if (r != 1) {
            fprintf(stderr, "[client] o servidor fechou a ligacao\n");
            return -1;
        }

        show_line(line, len, &reply);       /* mostra e faz o parse */
        if (reply.type == MT_OK)
            return 0;
        if (reply.type == MT_ERROR && reply.code == 409) {
            printf("[client] o nick %s ja esta em uso; escolhe outro\n", nick->nick);
            if (ask_nick(nick->nick, sizeof nick->nick) != 0)
                return -1;
            goto again;
        }
        if (reply.type == MT_ERROR)         /* nick recusado: não continuar */
            return -1;
    }
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

    /* Timeout SÓ para a resposta ao NICK: impede ficar preso para sempre
     * contra um servidor que não responda. É retirado a seguir — a thread de
     * leitura tem mesmo de ficar à espera indefinidamente. */
    struct timeval tv = { .tv_sec = REPLY_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    rxbuf_init(&rx);
    int status = EXIT_FAILURE;

    /* 1. NICK tem de ser a primeira mensagem (slide 124). */
    if (do_nick(fd, &nick) != 0)
        goto out;

    tv.tv_sec = 0;                          /* sem timeout a partir daqui */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    printf("[client] ligado a %s:%s como %s (/help para os comandos, Ctrl+D para sair)\n",
           host, port, nick.nick);

    /* 2. A partir daqui os dois sentidos são independentes. */
    pthread_t reader;
    if (pthread_create(&reader, NULL, reader_thread, &fd) != 0) {
        fprintf(stderr, "[client] nao foi possivel criar a thread de leitura\n");
        goto out;
    }

    /* 3. Teclado -> MSG/PRIV/LIST/QUIT. Sem esperar resposta: ela aparece
     *    quando chegar, pela mão da outra thread. */
    /* texto + "/msg " + nick + ' ' + "\r\n" + '\0' */
    char typed[PROTO_TEXT_MAX + PROTO_NICK_MAX + 16];
    int  sent_quit = 0;
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
        if (r == UI_NONE)
            continue;                       /* Enter sozinho */
        if (r == UI_HELP) {
            printf("%s\n", UI_HELP_TEXT);
            continue;
        }
        if (r == UI_BAD) {
            printf("[nao enviado] %s\n", why);
            continue;
        }

        if (m.type == MT_QUIT) {
            /* Antes de enviar: o fecho que o servidor faz a seguir ao OK já
             * é o fim esperado, não uma queda. */
            quitting = 1;
            sent_quit = 1;
            send_msg(fd, &m);
            break;
        }

        /* Sem eco local: o servidor devolve-nos o nosso CHAT (D5) e a cópia
         * de cada PRIV (D14). Se o imprimíssemos também, aparecia duplicado. */
        if (send_msg(fd, &m) != 0)
            break;
    }

    /* 4. Fim (/quit ou EOF no teclado). No EOF manda-se também o QUIT (D17):
     *    o servidor responde OK e fecha. Depois, fechar o sentido de envio
     *    (FIN): mesmo um servidor que ignore o QUIT vê recv()==0 e fecha. A
     *    thread de leitura mostra o que ainda chegar e termina ao ver o
     *    fecho. */
    quitting = 1;
    if (!sent_quit) {
        struct msg q = { .type = MT_QUIT };
        send_msg(fd, &q);                   /* se falhar, o fecho chega na mesma */
    }
    shutdown(fd, SHUT_WR);
    pthread_join(reader, NULL);
    status = EXIT_SUCCESS;

out:
    close(fd);
    return status;
}
