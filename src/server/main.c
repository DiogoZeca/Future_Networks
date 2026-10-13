/*
 * M2 — servidor de chat multiutilizador: uma thread por cliente.
 *
 * Por cada ligação, numa thread própria:
 *
 *   recv() --bytes--> rxbuf (framing) --linhas--> session (protocol + estado)
 *                                                     |
 *                                            saídas + destino
 *                                                     |
 *   send_all() <--linha JSON-- proto_encode() <-- deliver() -> roster
 *
 * Toda a lógica está nas camadas testadas à unidade; este ficheiro é a
 * "canalização" entre elas, os sockets e o roster.
 *
 *
 * O QUE MUDOU DO M1
 * -----------------
 * No M1 o servidor era ITERATIVO: o accept() devolvia uma ligação, servia-a
 * até ao fim, e só depois voltava a aceitar. Um segundo cliente completava o
 * handshake TCP — o kernel trata disso sozinho — mas ficava na fila do
 * listen() sem nunca ser servido (medimo-lo no M0 com o probe_m0.py).
 *
 * Agora o accept() lança uma thread e volta imediatamente ao ciclo.
 *
 *
 * AS TRÊS REGRAS DE CONCORRÊNCIA
 * ------------------------------
 * 1. O broadcast tira um RETRATO da lista dentro do lock e envia FORA dele.
 *    Com o lock agarrado, um cliente que não lê (portátil suspenso, Ctrl+S no
 *    terminal) encheria o buffer de envio, o send() bloquearia, e o chat
 *    inteiro parava — §2.4 e §3.2 do docs/typical-problems.md.
 *
 * 2. O retrato incrementa o refcount de cada cliente, e deliver() larga-o no
 *    fim. Sem isso, um cliente que saia entre o retrato e o envio seria
 *    libertado, o fd fechado, e o número do fd REUTILIZADO pelo kernel na
 *    ligação seguinte: a mensagem iria para o cliente errado, sem crash e sem
 *    erro nenhum.
 *
 * 3. Cada escrita num socket leva o wlock DESSE cliente. send() é atómico, mas
 *    o ciclo do send_all() não é: duas threads a escrever ao mesmo tempo no
 *    mesmo fd intercalam metade de uma mensagem com metade de outra, e o
 *    cliente recebe JSON corrompido (§3.3). O wlock é por cliente, por isso um
 *    cliente lento só atrasa quem lhe escreve, não o resto do chat.
 *
 * A mensagem é codificada UMA vez e os mesmos bytes vão para toda a gente:
 * é mais barato e garante que todos recebem exatamente o mesmo.
 *
 * Uso: ./bin/server [porta]
 */
#include "framing.h"
#include "net.h"
#include "protocol.h"
#include "roster.h"
#include "session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   /* strerror */
#include <unistd.h>

#define DEFAULT_PORT "5000"

/* A lista de clientes é global porque é o que as threads partilham. Toda a
 * concorrência que lhe diz respeito está dentro do roster.c. */
static struct roster R;

/* ---- Envio -------------------------------------------------------------- */

/* Escreve uma linha já codificada num cliente, com o wlock dele.
 * Devolve 0, ou -1 se a ligação morreu (o chamador decide o que fazer). */
static int send_line_to(struct client *c, const char *line, size_t len)
{
    pthread_mutex_lock(&c->wlock);
    int rc = send_all(c->fd, line, len);
    pthread_mutex_unlock(&c->wlock);
    return rc;
}

/* Entrega uma mensagem ao destino que a session indicou.
 *
 * `from` é quem provocou a mensagem — serve para o SESS_SELF e para excluir o
 * próprio nos SESS_OTHERS. Devolve -1 só quando o envio PARA O PRÓPRIO falha:
 * é o único caso que obriga a fechar esta ligação. Um erro a enviar para outro
 * cliente é problema da thread dele, que o vai descobrir no seu próprio recv(). */
static int deliver(struct client *from, const struct sess_out *o)
{
    char   line[PROTO_LINE_BUF];
    size_t len;

    /* Codificar uma vez só, antes de qualquer envio. */
    if (proto_encode(&o->msg, line, sizeof line, &len) != 0) {
        /* Inalcançável: a session só produz mensagens válidas e os testes
         * verificam-no caso a caso. Se acontecer, é um bug NOSSO. */
        fprintf(stderr, "[server] %s: BUG: %s nao codificavel\n",
                from->who, proto_type_name(o->msg.type));
        return -1;
    }

    if (o->dest == SESS_SELF)
        return send_line_to(from, line, len);

    /* Retrato DENTRO do lock (regra 1), com refcount (regra 2). */
    struct client *snap[ROSTER_MAX];
    size_t n = roster_snapshot(&R, snap, ROSTER_MAX,
                               o->dest == SESS_OTHERS ? from : NULL);

    /* Envio FORA do lock. Um send() que falhe aqui não é tratado: o peer está
     * morto e a thread dele vai dar por isso no recv(). */
    for (size_t i = 0; i < n; i++)
        send_line_to(snap[i], line, len);

    /* Tantos release quantos o snapshot devolveu — um a menos é uma fuga de
     * fds, um a mais é um use-after-free. */
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    return 0;
}

/* ---- Uma ligação -------------------------------------------------------- */

/* Serve uma ligação até ela fechar. Os buffers são locais da thread: ~20 KB
 * numa stack de 8 MB, e assim cada cliente tem os seus sem alocação nenhuma. */
static void serve(struct client *c, struct session *sess)
{
    struct rxbuf    rx;
    char            chunk[RX_CHUNK];
    char            line[RX_LINE_BUF];
    struct sess_out outs[SESS_MAX_OUT];
    /* Local da thread, logo sem lock: só esta thread muda o estado desta
     * sessão. Evita uma chamada com lock por cada mensagem recebida. */
    int             joined = 0;

    rxbuf_init(&rx);

    for (;;) {
        /* Nunca mais do que RX_CHUNK por recv(): é essa a disciplina que
         * garante que o rxbuf não transborda (framing.md §3.1). */
        ssize_t n = recv_retry(c->fd, chunk, sizeof chunk);
        if (n == 0) {
            printf("[server] %s fechou a ligacao\n", c->who);
            return;
        }
        if (n < 0) {
            /* ECONNRESET é o cliente a desaparecer à bruta (kill -9, cabo
             * arrancado com a pilha TCP ainda viva), não um erro nosso. */
            if (errno == ECONNRESET)
                printf("[server] %s: ligacao abortada pelo peer (RST)\n", c->who);
            else
                perror("recv");
            return;
        }

        if (rxbuf_feed(&rx, chunk, (size_t)n) != 0) {
            fprintf(stderr, "[server] %s: BUG: rxbuf_feed falhou\n", c->who);
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
                struct sess_out e = { .dest = SESS_SELF };
                e.msg.type = MT_ERROR;
                e.msg.code = 400;
                snprintf(e.msg.reason, sizeof e.msg.reason, "%s", SESS_R_TOOLONG);
                deliver(c, &e);
                /* Fechar sem ressincronizar (D2). Atenção: se ainda houver
                 * bytes deles por ler, o close() manda RST em vez de FIN
                 * (M0.md, nota 4) e o cliente pode nem chegar a ler o ERROR.
                 * Resolver isso a sério precisa de timeouts — M4. */
                return;
            }

            if (r != RX_LINE) {                 /* RX_FAULT: culpa NOSSA */
                fprintf(stderr, "[server] %s: BUG: rxbuf_next_line = %d\n", c->who, r);
                return;
            }

            size_t nout = session_handle(sess, line, len, outs, SESS_MAX_OUT);

            /* Entrar no chat ANTES de entregar as saídas: a ordem garante que
             * o EVENT "x joined" já sai com o x na lista — não para ele (vai
             * com SESS_OTHERS), mas para que um CHAT que chegue no mesmo
             * instante não o salte. */
            if (!joined && sess->state == SESS_READY) {
                roster_join(&R, c, sess->nick);
                joined = 1;
            }

            for (size_t i = 0; i < nout; i++) {
                if (outs[i].msg.type == MT_ERROR)
                    printf("[server] %s -> ERROR %d (%s)\n",
                           c->who, outs[i].msg.code, outs[i].msg.reason);
                if (deliver(c, &outs[i]) != 0)
                    return;                     /* só falha no envio ao próprio */
            }
        }
    }
}

/* O corpo de cada thread. Recebe a referência que o roster_add() lhe deu. */
static void *client_thread(void *arg)
{
    struct client *c = arg;
    struct session sess;

    session_init(&sess);
    serve(c, &sess);

    /* Tirar da lista ANTES de anunciar a saída: assim o retrato do EVENT já
     * não o inclui, e não há hipótese de alguém receber uma mensagem de um
     * cliente que já se foi embora. */
    roster_remove(&R, c);

    struct msg ev;
    if (session_leave(&sess, &ev)) {            /* 0 se saiu antes do NICK */
        struct sess_out o = { .dest = SESS_OTHERS, .msg = ev };
        deliver(c, &o);
        printf("[server] %s (%s) saiu\n", c->who, sess.nick);
    } else {
        printf("[server] %s desligou sem nick\n", c->who);
    }

    /* A última referência desta thread. Fecha o fd e liberta — a não ser que
     * um broadcast ainda tenha o cliente no retrato, caso em que só desaparece
     * quando esse largar a referência. */
    client_release(&R, c);
    printf("[server] %zu cliente(s) ligado(s)\n", roster_count(&R));
    return NULL;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *port = (argc > 1) ? argv[1] : DEFAULT_PORT;

    /* Mostrar cada linha de log logo, mesmo com o stdout redirecionado
     * para um ficheiro ou pipe (por omissão aí só sai em blocos de 4 KB). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    roster_init(&R);

    int listener = tcp_listen(port, 8);
    if (listener < 0)
        return EXIT_FAILURE;
    printf("[server] a escuta na porta %s (max %d clientes)\n", port, ROSTER_MAX);

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

        struct client *c = roster_add(&R, fd, who);
        if (c == NULL) {
            /* Lista cheia ou sem memória: recusar em condições em vez de
             * aceitar uma ligação que não conseguimos servir. */
            fprintf(stderr, "[server] %s recusado: lista cheia\n", who);
            close(fd);
            continue;
        }

        pthread_t tid;
        /* Detached: ninguém faz join, e sem isto os recursos de cada thread
         * nunca seriam libertados — uma fuga por cada cliente que sai. */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        int rc = pthread_create(&tid, &attr, client_thread, c);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            fprintf(stderr, "[server] pthread_create: %s\n", strerror(rc));
            roster_remove(&R, c);
            client_release(&R, c);              /* fecha o fd */
            continue;
        }

        printf("[server] ligado: %s (%zu cliente(s))\n", who, roster_count(&R));
    }

    close(listener);
    roster_destroy(&R);
    return EXIT_SUCCESS;
}
