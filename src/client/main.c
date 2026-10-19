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
 * O QUE MUDOU NO M4
 * ----------------
 *   - Keepalive (D2): o servidor desliga quem não manda nada durante 300 s
 *     (408, D23). Um utilizador pode estar só a ler muito mais do que isso.
 *     Por isso, se o cliente não enviou nada há KEEPALIVE_S (120 s), manda
 *     um LIST "silencioso" — sem tipos novos, que os outros servidores não
 *     conheceriam — e não mostra o USERS que vem de volta.
 *   - Servidor morto: se um desses LIST ficar sem resposta durante mais
 *     KEEPALIVE_S, o servidor (ou a rede) desapareceu sem FIN nem RST — a
 *     mesma "morte silenciosa" que o idle timeout apanha do lado do servidor.
 *     O cliente avisa e termina, em vez de ficar à espera para sempre.
 *   - Agora há duas threads a escrever no socket (a principal e a do
 *     keepalive): o send_msg() passou a ter um lock (send_lock), como o
 *     wlock de cada cliente no servidor.
 *   - Ctrl+C (e SIGTERM/SIGHUP): sai como o /quit — manda QUIT e espera o
 *     fecho. Um segundo Ctrl+C termina logo (term.c).
 *
 *
 * O QUE MUDOU NO M5 (slide 123: "the client finds servers on the LAN with no
 * address typed in")
 * ---------------------------------------------------------------------------
 *   - Sem endereço (`./bin/client`), o cliente PROCURA servidores: manda um
 *     DISCOVER por broadcast UDP (porta 5001), junta as respostas durante
 *     1,5 s, mostra-as e liga-se ao escolhido (discover.c). Um só → liga-se
 *     logo. Nenhum → diz porquê pode ter falhado (firewall!) e sai.
 *   - O IP a que se liga é o de ORIGEM da resposta, nunca um que venha na
 *     mensagem; a porta é a "tcp_port" (ou "port") da resposta.
 *   - Depois de escolher, tudo como antes: o nick, o NICK, o chat.
 *   - Com endereço, nada muda (as opções -u, -w e -b só contam sem ele).
 *
 * Uso: ./bin/client [-k s] [-u porta UDP] [-w ms] [-b ip]... [<host> [porta] [nick]]
 *   -k  keepalive (por omissão 120; 0 desliga)   sem nick, é pedido ao arrancar
 *   sem host, procura servidores na rede:
 *   -u  porta UDP da descoberta (5001)  -w  espera pelas respostas (1500 ms)
 *   -b  para onde mandar o DISCOVER em vez do broadcast (repetível; ex.:
 *       -b 127.255.255.255 só nesta máquina, -b 192.168.1.20 um servidor só)
 */
#include "discover.h"
#include "framing.h"
#include "net.h"
#include "protocol.h"
#include "term.h"
#include "ui.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT     "5000"
#define REPLY_TIMEOUT_S  5          /* só para a resposta ao NICK */
#define KEEPALIVE_S      120        /* M4: bem abaixo dos 300 s do servidor */

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
static int users_is_silent(void);         /* M4, mais abaixo */

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
    if (m->type == MT_USERS && users_is_silent())
        return;                             /* resposta a um keepalive (M4) */
    ui_format(m, text, sizeof text);
    term_out(stdout, "%s", text);
}

/* ---- M4: o keepalive e os LIST pendentes -------------------------------
 *
 * Cada LIST enviado espera um USERS, e os USERS chegam pela ordem dos LIST
 * (uma ligação TCP, um servidor que responde por ordem). Uma fila guarda, por
 * cada LIST ainda sem resposta, se foi do utilizador (mostrar o USERS) ou do
 * keepalive (não mostrar). Sem a fila, um /list do utilizador logo a seguir a
 * um keepalive podia ficar escondido no lugar do outro. */
#define PENDING_MAX 32

static pthread_mutex_t lists_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char   pending[PENDING_MAX];  /* 1 = silencioso */
static size_t          pending_head = 0, pending_n = 0;
static long            ka_since     = 0;      /* ms do keepalive por responder (0 = nenhum) */

/* Duas threads escrevem no socket (a principal e a do keepalive). */
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static long            last_sent = 0;         /* ms do último envio (send_lock) */

static long keepalive_ms = KEEPALIVE_S * 1000L;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Chegou um USERS: era a resposta a um LIST silencioso? (Tira-o da fila.) */
static int users_is_silent(void)
{
    pthread_mutex_lock(&lists_lock);
    int silent = 0;
    if (pending_n > 0) {
        silent = pending[pending_head];
        pending_head = (pending_head + 1) % PENDING_MAX;
        pending_n--;
        if (silent)
            ka_since = 0;                     /* o servidor está vivo */
    }
    pthread_mutex_unlock(&lists_lock);
    return silent;
}

/* Codifica e envia, com o send_lock JÁ agarrado. Um LIST entra na fila ANTES
 * de sair (a resposta não pode chegar antes de ele lá estar). -1 = sair. */
static int send_msg_locked(int fd, const struct msg *m, int silent)
{
    char   line[PROTO_LINE_BUF];
    size_t len;

    if (proto_encode(m, line, sizeof line, &len) != 0) {
        term_out(stderr, "[client] BUG: mensagem nao codificavel");
        return -1;
    }
    if (m->type == MT_LIST) {
        pthread_mutex_lock(&lists_lock);
        if (pending_n < PENDING_MAX) {
            pending[(pending_head + pending_n) % PENDING_MAX] = (unsigned char)silent;
            pending_n++;
            if (silent)
                ka_since = now_ms();
        }
        pthread_mutex_unlock(&lists_lock);
    }
    last_sent = now_ms();
    if (send_all(fd, line, len) < 0) {
        term_out(stderr, "send: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int send_msg(int fd, const struct msg *m)
{
    pthread_mutex_lock(&send_lock);
    int rc = send_msg_locked(fd, m, 0);
    pthread_mutex_unlock(&send_lock);
    return rc;
}

/* A thread do keepalive. Acorda de vez em quando (ou quando se sai) e:
 *   - se um LIST silencioso está por responder há keepalive_ms: o servidor
 *     morreu sem avisar → avisar e terminar;
 *   - se não se envia nada há keepalive_ms: mandar um LIST silencioso. */
static pthread_mutex_t ka_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ka_cond;

static void *keepalive_thread(void *arg)
{
    int  fd   = *(int *)arg;
    long step = keepalive_ms / 4 < 1000 ? keepalive_ms / 4 : 1000;

    pthread_mutex_lock(&ka_lock);
    while (!quitting) {
        struct timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec  += step / 1000;
        until.tv_nsec += (step % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&ka_cond, &ka_lock, &until);
        if (quitting)
            break;

        long now = now_ms();
        pthread_mutex_lock(&lists_lock);
        long since = ka_since;
        pthread_mutex_unlock(&lists_lock);
        if (since != 0 && now - since >= keepalive_ms) {
            term_out(stderr, "[client] o servidor nao responde ha %ld s: a sair",
                     (now - since) / 1000);
            term_exit(EXIT_FAILURE);          /* NÃO exit(): ver term.h */
        }

        pthread_mutex_lock(&send_lock);
        if (!quitting && since == 0 && now - last_sent >= keepalive_ms) {
            struct msg l = { .type = MT_LIST };
            send_msg_locked(fd, &l, 1);       /* se falhar, o leitor vê o fecho */
        }
        pthread_mutex_unlock(&send_lock);
    }
    pthread_mutex_unlock(&ka_lock);
    return NULL;
}

/* ---- Thread de leitura -------------------------------------------------- */

/* Lê o socket até ao fim e mostra tudo o que chegar. Nunca escreve no socket.
 *
 * Se o servidor cair enquanto estamos à espera do teclado, a thread principal
 * está presa no read() (ou no fgets()) do teclado e não há forma portável de
 * a interromper — por isso esta thread avisa e termina o processo, com
 * term_exit() e não exit() (term.h: com o fgets() à espera, o exit() de uma
 * glibc recente bloqueava para sempre). Quando somos NÓS a sair (Ctrl+D), o
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
        term_exit(r == -2 ? EXIT_SUCCESS : EXIT_FAILURE);   /* NÃO exit(): ver term.h */
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

static void usage(const char *prog)
{
    fprintf(stderr, "uso: %s [-k segundos] [<host> [porta] [nick]]\n"
                    "  -k  keepalive em segundos (por omissao %d; 0 desliga)\n"
                    "  sem host, procura servidores na rede (UDP):\n"
                    "  -u  porta UDP da descoberta (por omissao %s)\n"
                    "  -w  quanto tempo esperar pelas respostas, em ms (por omissao %d)\n"
                    "  -b  mandar o pedido para este IPv4 em vez do broadcast (repetivel)\n",
            prog, KEEPALIVE_S, DISCO_PORT_DEFAULT, DISCOVER_WAIT_MS);
}

/* "-u 5001", "-w 1500": um inteiro em [lo, hi], ou -1. */
static long parse_num(const char *s, long lo, long hi)
{
    char *end;
    long  v = strtol(s, &end, 10);
    return (*s != '\0' && *end == '\0' && v >= lo && v <= hi) ? v : -1;
}

/* M5: sem host, procurar servidores e escolher um. Escreve o IP em ip[] e a
 * porta TCP em port[]. Devolve 0, ou -1 (já explicado ao utilizador). */
static int find_server(const struct discover_opts *o, char *ip, size_t ipcap,
                       char *port, size_t portcap)
{
    struct disco_list    found;
    struct discover_info info;

    printf("[client] a procurar servidores na rede (UDP %d, %ld ms)...\n",
           o->udp_port, o->wait_ms);
    if (discover(o, &found, &info) != 0) {
        fprintf(stderr, "[client] nao consegui procurar: %s\n", strerror(errno));
        return -1;
    }
    if (info.loopback_only)
        printf("[client] sem rede: so procurei nesta maquina\n");
    if (info.dropped > 0)
        printf("[client] mais servidores responderam do que cabem na lista (%d)\n",
               DISCO_SERVERS_MAX);

    if (found.n == 0) {
        fprintf(stderr,
                "[client] nenhum servidor respondeu. Possiveis razoes:\n"
                "  - nao ha nenhum servidor a correr nesta rede;\n"
                "  - a firewall do servidor bloqueia a porta UDP %d "
                "(Ubuntu: sudo ufw allow %d/udp);\n"
                "  - a firewall DESTA maquina bloqueia as respostas, que nao sao vistas como\n"
                "    resposta a um broadcast (Ubuntu: sudo ufw allow proto udp from any port %d to any);\n"
                "  - a rede nao deixa passar broadcasts (Wi-Fi com isolamento de clientes).\n"
                "  Se souberes o endereco: %s <ip> [porta] [nick]\n",
                o->udp_port, o->udp_port, o->udp_port, "./bin/client");
        return -1;
    }

    int k = discover_choose(&found);
    if (k < 0)
        return -1;                              /* EOF no teclado */
    snprintf(ip, ipcap, "%s", found.s[k].ip);
    snprintf(port, portcap, "%d", found.s[k].tcp_port);
    return 0;
}

int main(int argc, char *argv[])
{
    struct discover_opts dopt = { .udp_port = 0, .wait_ms = DISCOVER_WAIT_MS };
    dopt.udp_port = (int)parse_num(DISCO_PORT_DEFAULT, 1, 65535);

    int opt;
    while ((opt = getopt(argc, argv, "k:u:w:b:h")) != -1) {
        long v;
        struct in_addr a;
        switch (opt) {
        case 'k':
            if ((v = parse_num(optarg, 0, 86400)) < 0) {
                fprintf(stderr, "-k: segundos entre 0 e 86400\n");
                return EXIT_FAILURE;
            }
            keepalive_ms = v * 1000;
            break;
        case 'u':
            if ((v = parse_num(optarg, 1, 65535)) < 0) {
                fprintf(stderr, "-u: porta UDP entre 1 e 65535\n");
                return EXIT_FAILURE;
            }
            dopt.udp_port = (int)v;
            break;
        case 'w':
            if ((v = parse_num(optarg, 1, 60000)) < 0) {
                fprintf(stderr, "-w: milissegundos entre 1 e 60000\n");
                return EXIT_FAILURE;
            }
            dopt.wait_ms = v;
            break;
        case 'b':
            if (inet_pton(AF_INET, optarg, &a) != 1) {
                fprintf(stderr, "-b: tem de ser um endereco IPv4 (ex.: 192.168.1.255)\n");
                return EXIT_FAILURE;
            }
            if (dopt.n_targets == DISCOVER_TARGETS_MAX) {
                fprintf(stderr, "-b: no maximo %d\n", DISCOVER_TARGETS_MAX);
                return EXIT_FAILURE;
            }
            dopt.targets[dopt.n_targets++] = optarg;
            break;
        default:
            usage(argv[0]);
            return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    argc -= optind - 1;                     /* argv[1] passa a ser o host */
    argv += optind - 1;

    setvbuf(stdout, NULL, _IOLBF, 0);       /* ver cada linha logo, mesmo em pipe */

    /* M5: sem host, procurar. Os buffers são static: host e port apontam
     * para eles até ao fim. */
    static char found_ip[DISCO_IP_BUF], found_port[8];
    const char *host, *port;
    if (argc < 2) {
        if (find_server(&dopt, found_ip, sizeof found_ip, found_port, sizeof found_port) != 0)
            return EXIT_FAILURE;
        host = found_ip;
        port = found_port;
    } else {
        host = argv[1];
        port = (argc > 2) ? argv[2] : DEFAULT_PORT;
    }

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
    pthread_t reader, keeper;
    int       keeping = 0;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&ka_cond, &ca);
    pthread_condattr_destroy(&ca);

    pthread_mutex_lock(&send_lock);
    last_sent = now_ms();                   /* o NICK acabou de sair */
    pthread_mutex_unlock(&send_lock);

    term_hold_signals();
    int rc = pthread_create(&reader, NULL, reader_thread, &fd);
    if (rc == 0 && keepalive_ms > 0)        /* M4: -k 0 desliga */
        keeping = pthread_create(&keeper, NULL, keepalive_thread, &fd) == 0;
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
    if (term_quit_requested())
        term_out(stderr, "[client] Ctrl+C: a sair (QUIT)");
    quitting = 1;
    if (keeping) {                          /* parar o keepalive antes do QUIT */
        pthread_mutex_lock(&ka_lock);
        pthread_cond_signal(&ka_cond);
        pthread_mutex_unlock(&ka_lock);
        pthread_join(keeper, NULL);
    }
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
