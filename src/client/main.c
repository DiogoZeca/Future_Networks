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
 * PORQUE É QUE HÁ SÓ UM LOCK AQUI (O DO ECRÃ)
 * -------------------------------------------
 * Uma ligação TCP é full-duplex: os dois sentidos são independentes. Como cada
 * thread usa um sentido só — a principal escreve, a de leitura lê — o socket
 * não precisa de lock nenhum:
 *
 *   rx (rxbuf)   só a thread de leitura lhe toca, depois do arranque
 *   send_all()   só a thread principal o chama
 *
 * É por isso que o servidor precisa de um wlock por cliente e o cliente não:
 * lá há N threads a escrever no mesmo socket, aqui há uma.
 *
 * O que as duas threads PARTILHAM é o ecrã: a principal desenha a linha que
 * se está a escrever, a de leitura mostra o que chega. Até à correção do M3
 * bastava que cada printf() fosse atómico; desde que o cliente desenha ele
 * próprio a linha de escrita (term.h), mostrar uma mensagem é "apagar a linha,
 * escrever, redesenhar", e isso tem de ser uma operação só. Por isso TUDO o
 * que se mostra passa por term_out(), que tem o mutex do ecrã.
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
 * REVISÃO DO M3 (acordo com o primeiro grupo)
 * -------------------------------------------
 *   - Os OK não se mostram. O servidor manda OK a cada pedido aceite (D13),
 *     mas o cliente não espera por eles: envia e continua a ler o teclado. Só
 *     o OK ao NICK interessa (do_nick); os outros são ignorados quando chegam.
 *   - Eco LOCAL do /msg: o servidor já não devolve cópia do PRIV (D14), por
 *     isso o cliente mostra ele próprio "<ana -> rui> (privado) ...". Se o
 *     destino não existir, o ERROR 404 aparece logo a seguir.
 *   - Tolerante a receber: uma linha maior do que 64 KiB vinda do servidor é
 *     DESCARTADA com um aviso (rxbuf_init_lenient), em vez de fechar o
 *     cliente; texto de várias linhas é mostrado indentado e limpo (ui.c).
 *
 * CORREÇÃO DO M3 (depois do teste cruzado)
 * ----------------------------------------
 *   - Num terminal, o cliente desenha ele próprio a linha de escrita, com o
 *     nick à frente ("ana> ..."), em vez de deixar o terminal fazer o eco
 *     (term.c, ui_line_* em ui.c). Uma mensagem que chega já não se mistura
 *     com o que se está a escrever; o que se envia aparece uma vez só (a
 *     linha de escrita é apagada no Enter e fica o CHAT do servidor); o
 *     Enter sozinho não faz nada. Sem terminal (pipe) fica tudo como antes.
 *   - O servidor manda boas-vindas a quem entra (D22): é um EVENT, e
 *     mostra-se como os outros ("* welcome, ana! ...").
 *
 * Uso: ./bin/client <host> [porta] [nick]   (sem nick, é pedido ao arrancar)
 */
#include "framing.h"
#include "net.h"
#include "protocol.h"
#include "term.h"
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
        if (r == RX_SKIPPED) {              /* modo tolerante: avisar e seguir */
            term_out(stderr, "[client] o servidor mandou uma linha com mais de %d "
                             "bytes: descartada", RX_MAX_LINE);
            continue;
        }
        if (r != RX_NEED) {                 /* RX_FAULT: bug nosso */
            term_out(stderr, "[client] BUG: rxbuf_next_line = %d", r);
            return -1;
        }

        ssize_t n = recv_retry(fd, chunk, sizeof chunk);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;                       /* passou o SO_RCVTIMEO */
        if (n == 0)
            return -2;
        if (n < 0) {
            if (!quitting)
                term_out(stderr, "recv: %s", strerror(errno));
            return -1;
        }
        if (rxbuf_feed(&rx, chunk, (size_t)n) != 0)
            return -1;                      /* inalcançável com recv <= RX_CHUNK */
    }
}

/* Mostra uma linha recebida. Devolve a mensagem em *m (type 0 se inválida).
 *
 * Os OK não se mostram (revisão do M3): chegam a cada pedido aceite e não
 * dizem nada ao utilizador. Quem precisa de um (do_nick) olha para *m. */
static void show_line(const char *line, size_t len, struct msg *m)
{
    struct proto_err err;
    static char      text[PROTO_LINE_BUF];   /* 64 KiB: fora da stack */

    if (proto_parse(line, len, m, &err) != 0) {
        /* Ex.: um servidor de outro grupo com uma extensão que não
         * conhecemos. Mostrar em bruto em vez de rebentar ou esconder — mas
         * curto e sem caracteres de controlo (podiam mexer no terminal). */
        char   bruto[201];
        size_t k = 0;
        for (; k < len && k < 200; k++) {
            unsigned char c = (unsigned char)line[k];
            bruto[k] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
        }
        bruto[k] = '\0';
        term_out(stdout, "[servidor mandou mensagem nao suportada (%s)] %s%s",
                 err.reason, bruto, len > 200 ? "..." : "");
        return;
    }
    if (m->type == MT_OK)
        return;
    ui_format(m, text, sizeof text);
    term_out(stdout, "%s", text);
}

/* Codifica e envia. -1 = sair. */
static int send_msg(int fd, const struct msg *m)
{
    char   line[PROTO_LINE_BUF];
    size_t len;

    if (proto_encode(m, line, sizeof line, &len) != 0) {
        term_out(stderr, "[client] BUG: mensagem nao codificavel");
        return -1;
    }
    if (send_all(fd, line, len) < 0) {
        term_out(stderr, "send: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- Thread de leitura -------------------------------------------------- */

/* Lê o socket até ao fim e mostra tudo o que chegar. Nunca escreve no socket.
 *
 * Se o servidor cair enquanto estamos à espera do teclado, a thread principal
 * está presa no read() (ou no fgets()) do teclado e não há forma portável de
 * a interromper — por isso esta thread avisa e termina o processo (o atexit
 * repõe o terminal). Quando somos NÓS a sair (Ctrl+D), o
 * fecho é esperado: basta retornar e deixar o join acontecer. */
static void *reader_thread(void *arg)
{
    int         fd = *(int *)arg;
    static char line[RX_LINE_BUF];           /* 64 KiB: fora da stack da thread */
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
            term_out(stderr, "[client] o servidor fechou a ligacao");
        exit(r == -2 ? EXIT_SUCCESS : EXIT_FAILURE);   /* o atexit repõe o terminal */
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
        term_out(stdout, "[nao enviado] %s", UI_WHY_BADNICK);
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
            term_out(stderr, "[client] sem resposta do servidor em %d s", REPLY_TIMEOUT_S);
            return -1;
        }
        if (r != 1) {
            term_out(stderr, "[client] o servidor fechou a ligacao");
            return -1;
        }

        show_line(line, len, &reply);       /* mostra e faz o parse */
        if (reply.type == MT_OK)
            return 0;
        if (reply.type == MT_ERROR && reply.code == 409) {
            term_out(stdout, "[client] o nick %s ja esta em uso; escolhe outro", nick->nick);
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

    /* Tolerante: uma linha longa de mais vinda do servidor é descartada, não
     * fecha o cliente (o servidor, esse, continua estrito: D2). */
    rxbuf_init_lenient(&rx);
    int status = EXIT_FAILURE;

    /* 1. NICK tem de ser a primeira mensagem (slide 124). */
    if (do_nick(fd, &nick) != 0)
        goto out;

    tv.tv_sec = 0;                          /* sem timeout a partir daqui */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    term_out(stdout, "[client] ligado a %s:%s como %s (/help para os comandos, Ctrl+D para sair)",
             host, port, nick.nick);

    /* 2. Num terminal: desenhar nós a linha de escrita (term.h). Só agora,
     *    com o nick final (pode ter mudado depois de um 409). */
    term_start(nick.nick);

    /* 3. A partir daqui os dois sentidos são independentes. A thread de
     *    leitura nasce com os sinais do terminal bloqueados (herda a máscara):
     *    assim chegam sempre à thread principal, que é a que está no read()
     *    do teclado e sabe redesenhar. */
    pthread_t reader;
    term_hold_signals();
    int rc = pthread_create(&reader, NULL, reader_thread, &fd);
    term_release_signals();
    if (rc != 0) {
        term_stop();
        fprintf(stderr, "[client] nao foi possivel criar a thread de leitura\n");
        goto out;
    }

    /* 4. Teclado -> MSG/PRIV/LIST/QUIT. Sem esperar resposta: ela aparece
     *    quando chegar, pela mão da outra thread. */
    /* "/msg " + nick + ' ' + texto + "\r\n" + '\0' */
    char typed[UI_LINE_MAX + 3];
    int  sent_quit = 0;
    for (;;) {
        int k = term_readline(typed, sizeof typed);
        if (k == TERM_EOF || k == TERM_ERR)
            break;
        if (k == TERM_TOOLONG) {            /* só sem terminal */
            term_out(stdout, "[nao enviado] %s", UI_WHY_TOOLONG);
            continue;
        }

        struct msg  m;
        const char *why;
        int r = ui_input_to_msg(typed, &m, &why);
        if (r == UI_NONE)
            continue;                       /* Enter sozinho (sem terminal) */
        if (r == UI_HELP) {
            term_out(stdout, "%s", UI_HELP_TEXT);
            continue;
        }
        if (r == UI_BAD) {
            term_out(stdout, "[nao enviado] %s", why);
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

        /* MSG: sem eco local, o servidor devolve-nos o nosso CHAT (D5) — se o
         * imprimíssemos também, aparecia duplicado. E num terminal a linha
         * escrita já foi apagada no Enter (term_readline): o que fica no ecrã
         * é só esse CHAT. */
        if (send_msg(fd, &m) != 0)
            break;

        /* PRIV: o servidor NÃO devolve cópia (D14, revisão do M3), por isso o
         * eco é local. Um CHAT com `to` preenchido nunca vem do fio, e o
         * ui_format mostra-o como "<eu -> destino> (privado) ...". */
        if (m.type == MT_PRIV) {
            static struct msg echo;
            static char       shown[PROTO_LINE_BUF];
            memset(&echo, 0, sizeof echo);
            echo.type       = MT_CHAT;
            echo.is_private = 1;
            snprintf(echo.from, sizeof echo.from, "%s", nick.nick);
            memcpy(echo.to,   m.to,      sizeof echo.to);
            memcpy(echo.text, m.text,    sizeof echo.text);
            ui_format(&echo, shown, sizeof shown);
            term_out(stdout, "%s", shown);
        }
    }

    /* 4. Fim (/quit ou EOF no teclado). No EOF manda-se também o QUIT (D17):
     *    o servidor responde OK e fecha. Depois, fechar o sentido de envio
     *    (FIN): mesmo um servidor que ignore o QUIT vê recv()==0 e fecha. A
     *    thread de leitura mostra o que ainda chegar e termina ao ver o
     *    fecho. */
    quitting = 1;
    /* O terminal volta ao normal JÁ: o que ainda chegar até o servidor fechar
     * aparece sem prompt (já não se está a escrever nada). */
    term_stop();
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
