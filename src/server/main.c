/*
 * M3 — servidor de chat multiutilizador: uma thread por cliente, com os
 * comandos do slide 124 (PRIV, LIST, QUIT) e nicks únicos.
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
 * AS QUATRO REGRAS DE CONCORRÊNCIA
 * --------------------------------
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
 *    cliente recebe JSON corrompido (§3.3).
 *
 * 4. Nenhum envio pode bloquear para sempre. As regras 1-3 tiram o send() de
 *    dentro do lock do roster, mas NÃO chegam: num chat com broadcast toda a
 *    gente escreve a toda a gente, e um cliente que não lê (portátil
 *    suspenso, Ctrl+Z, Ctrl+S) deixava presa a thread de quem lhe escrevesse
 *    — e, atrás dela, todas as outras à espera do wlock dele. O chat inteiro
 *    congelava (medido: ~2,6 MB depois). Por isso cada envio tem um PRAZO
 *    (SEND_TIMEOUT_MS, send_all_timeout()); quem o esgota é marcado `dead` e
 *    desligado, e ninguém volta a tentar escrever-lhe (§2.4, slide 125:
 *    "A slow or stuck client blocks everyone. Use timeouts").
 *
 * A mensagem é codificada UMA vez e os mesmos bytes vão para toda a gente:
 * é mais barato e garante que todos recebem exatamente o mesmo.
 *
 *
 * O QUE MUDOU DO M2 PARA O M3
 * ---------------------------
 * Pouco, e de propósito: as decisões novas (409, 404, USERS, QUIT) estão
 * todas na session. Aqui há só três coisas novas:
 *
 *   - o DIRETÓRIO de cada ligação (dir_claim/dir_lookup/dir_nicks): as
 *     perguntas da session ("está livre?", "existe?", "quem está?")
 *     respondidas pelo roster;
 *   - o destino SESS_TO (PRIV): roster_find() com refcount, como o retrato
 *     (regra 2), e o envio fora do lock com prazo (regras 1 e 4);
 *   - o QUIT: depois de enviar o OK, a session fica CLOSED e o serve()
 *     termina — a saída é a mesma de uma queda (roster_remove + "x left").
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

#define DEFAULT_PORT    "5000"

/* Quanto tempo um cliente pode estar sem ler antes de ser desligado. Tem de
 * ser muito maior do que qualquer atraso normal (um portátil em Wi-Fi fraco)
 * e curto o suficiente para o resto do chat não dar pela pausa como avaria. */
#define SEND_TIMEOUT_MS 5000

#define SRV_R_FULL "server full"   /* ERROR 503, PROTOCOL.md §5 */

/* A lista de clientes é global porque é o que as threads partilham. Toda a
 * concorrência que lhe diz respeito está dentro do roster.c. */
static struct roster R;

/* ---- Envio -------------------------------------------------------------- */

/* Escreve uma linha já codificada num cliente, com o wlock dele e com prazo
 * (regras 3 e 4). Devolve 0, ou -1 se este cliente já não pode receber nada.
 *
 * Se o envio falhar ou o prazo se esgotar, o cliente é marcado `dead` e
 * desligado com shutdown():
 *
 *   - `dead` faz com que mais ninguém espere por ele: as threads que estavam
 *     na fila do wlock desistem logo, sem gastar outro prazo cada uma.
 *   - shutdown() e NÃO close(): o close() é do refcount (roster.h). O
 *     shutdown() só acorda o recv() da thread dona, que faz a saída normal —
 *     roster_remove() e o EVENT "x left" para os outros.
 *   - um envio interrompido pode ter deixado MEIA linha no fio
 *     (typical-problems.md §1.3). Não há forma de a "desfazer"; é por isso
 *     que a única saída é fechar e nunca voltar a escrever nesse socket. */
static int send_line_to(struct client *c, const char *line, size_t len)
{
    if (atomic_load(&c->dead))              /* já sabemos que não serve */
        return -1;

    pthread_mutex_lock(&c->wlock);
    /* Re-verificar dentro do lock: podemos ter estado na fila do wlock
     * enquanto outra thread esgotava o prazo neste mesmo cliente. */
    int rc = atomic_load(&c->dead) ? -1
           : send_all_timeout(c->fd, line, len, SEND_TIMEOUT_MS);
    if (rc != 0 && !atomic_exchange(&c->dead, 1)) {
        /* Só a primeira thread a falhar chega aqui (atomic_exchange). */
        if (rc == SEND_TIMEOUT_EXPIRED)
            printf("[server] %s nao le ha %d ms: a desligar\n",
                   c->who, SEND_TIMEOUT_MS);
        shutdown(c->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&c->wlock);
    return rc == 0 ? 0 : -1;
}

/* Entrega uma mensagem ao destino que a session indicou.
 *
 * `from` é quem provocou a mensagem — serve para o SESS_SELF e para excluir o
 * próprio nos SESS_OTHERS. Devolve -1 só quando o envio SESS_SELF falha: é o
 * único caso que obriga esta thread a parar já.
 *
 * Nos broadcasts os erros não são devolvidos, nem quando o próprio está no
 * retrato (SESS_ALL): quem falhou ficou marcado `dead` e com shutdown() feito
 * (send_line_to), e a thread DONO desse cliente dá por isso no recv() e trata
 * da saída. Assim um cliente morto nunca faz parar a thread de outro. */
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

    if (o->dest == SESS_TO) {
        /* PRIV (M3). find() dá +1 referência dentro do lock, pela mesma razão
         * do retrato (regra 2): o destino pode sair enquanto lhe enviamos.
         * Se já saiu desde que a session verificou que existia, a mensagem
         * perde-se sem erro — como um broadcast a quem saiu a meio. */
        struct client *to = roster_find(&R, o->msg.to);
        if (to == NULL) {
            printf("[server] %s: PRIV para %s, que entretanto saiu\n",
                   from->who, o->msg.to);
            return 0;
        }
        send_line_to(to, line, len);        /* falhar aqui é problema DELE */
        client_release(&R, to);
        return 0;
    }

    /* Retrato DENTRO do lock (regra 1), com refcount (regra 2). */
    struct client *snap[ROSTER_MAX];
    size_t n = roster_snapshot(&R, snap, ROSTER_MAX,
                               o->dest == SESS_OTHERS ? from : NULL);

    /* Envio FORA do lock (regra 1) e com prazo (regra 4). Um envio que falhe
     * aqui não é tratado: send_line_to() já desligou esse cliente. */
    for (size_t i = 0; i < n; i++)
        send_line_to(snap[i], line, len);

    /* Tantos release quantos o snapshot devolveu — um a menos é uma fuga de
     * fds, um a mais é um use-after-free. */
    for (size_t i = 0; i < n; i++)
        client_release(&R, snap[i]);

    return 0;
}

/* ---- O diretório de uma ligação (M3) ----------------------------------- */

/* As três perguntas da session (session.h), respondidas pelo roster. O ctx é
 * o próprio cliente: o claim tem de saber QUEM reserva. Cada uma é uma única
 * operação do roster, com o lock lá dentro — em especial o claim, onde
 * "verificar" e "reservar" não podem estar separados (test_roster #18). */
static int dir_claim(void *ctx, const char *nick)
{
    return roster_claim(&R, ctx, nick);
}

static int dir_lookup(void *ctx, const char *nick, char canon[PROTO_NICK_MAX + 1])
{
    (void)ctx;
    return roster_lookup(&R, nick, canon);
}

static size_t dir_nicks(void *ctx, char (*out)[PROTO_NICK_MAX + 1], size_t cap)
{
    (void)ctx;
    return roster_nicks(&R, out, cap);
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
                /* 413 e não 400 (revisão do M3, acordado com o primeiro
                 * grupo): 413 = "a ligação não está em condições e é
                 * fechada"; 400 = "pedido recusado, continua". */
                struct sess_out e = { .dest = SESS_SELF };
                e.msg.type = MT_ERROR;
                e.msg.code = 413;
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

            for (size_t i = 0; i < nout; i++) {
                if (outs[i].msg.type == MT_ERROR)
                    printf("[server] %s -> ERROR %d (%s)\n",
                           c->who, outs[i].msg.code, outs[i].msg.reason);
                if (deliver(c, &outs[i]) != 0)
                    return;                     /* só falha no envio ao próprio */

                /* Entrar no chat DEPOIS de o OK ter seguido (outs[0], SELF) e
                 * ANTES do EVENT (outs[1], OTHERS) — D9.
                 *
                 * Antes do OK não pode ser: entre o roster_join() e o envio do
                 * OK, um CHAT ou EVENT de outra thread passava à frente, e o
                 * cliente via outra coisa como resposta ao seu NICK (medido:
                 * 26 em 2000 ligações com tráfego constante). Como o OK já
                 * saiu por completo (enviado dentro do wlock), tudo o que
                 * vier a seguir chega depois dele.
                 *
                 * Um CHAT enviado no instante entre o OK e esta linha não
                 * chega a quem está a entrar — e está certo: ainda não estava
                 * no chat. */
                if (!joined && sess->state == SESS_READY) {
                    roster_join(&R, c, sess->nick);
                    joined = 1;
                }
            }

            /* QUIT (D17): o OK já seguiu. Parar de ler e sair como numa
             * queda — o client_thread() faz o roster_remove() e o "x left".
             * O que vier depois do QUIT, mesmo que já esteja no rxbuf, é
             * ignorado. O shutdown(SHUT_WR) manda já o FIN, a seguir ao OK:
             * o cliente vê o fecho sem esperar pelo close() do refcount.
             * (Se o cliente tiver mandado mais bytes depois do QUIT, o close()
             * final pode sair como RST — o mesmo problema do "line too long",
             * para o M4.) */
            if (sess->state == SESS_CLOSED) {
                printf("[server] %s (%s) pediu QUIT\n", c->who,
                       sess->nick[0] ? sess->nick : "sem nick");
                shutdown(c->fd, SHUT_WR);
                return;
            }
        }
    }
}

/* O corpo de cada thread. Recebe a referência que o roster_add() lhe deu. */
static void *client_thread(void *arg)
{
    struct client  *c   = arg;
    struct session  sess;
    struct sess_dir dir = { c, dir_claim, dir_lookup, dir_nicks };

    session_init(&sess, &dir);
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
            /* Um accept() falhado é quase sempre problema DESSA ligação
             * (ECONNABORTED: desistiu antes de ser aceite) ou passageiro
             * (EMFILE/ENFILE: sem fds livres, ENOBUFS/ENOMEM). Nenhum é razão
             * para deitar abaixo o chat de toda a gente — e sair daqui com
             * threads vivas tornava o roster_destroy() um use-after-free. */
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            if (errno == EMFILE || errno == ENFILE ||
                errno == ENOBUFS || errno == ENOMEM) {
                sleep(1);                       /* dar tempo a que saia alguém */
                continue;
            }
            break;                              /* EBADF, EINVAL...: bug nosso */
        }

        char who[64];
        addr_to_str((struct sockaddr *)&peer, who, sizeof who);

        struct client *c = roster_add(&R, fd, who);
        if (c == NULL) {
            /* Lista cheia ou sem memória: recusar em condições, com um ERROR
             * que o cliente consegue mostrar, em vez de um fecho sem
             * explicação (PROTOCOL.md §5). Prazo curto: não vale a pena
             * prender o ciclo do accept() por causa de quem vai ser recusado. */
            fprintf(stderr, "[server] %s recusado: lista cheia\n", who);
            struct msg e = { .type = MT_ERROR, .code = 503 };
            snprintf(e.reason, sizeof e.reason, "%s", SRV_R_FULL);
            char   line[PROTO_LINE_BUF];
            size_t len;
            if (proto_encode(&e, line, sizeof line, &len) == 0)
                send_all_timeout(fd, line, len, 200);
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

    /* Só se chega aqui por um bug nosso (accept() com EBADF/EINVAL). Pode
     * haver threads vivas, por isso NÃO se chama roster_destroy() (seria um
     * use-after-free, ver roster.h); o encerramento limpo é do M4 (Ctrl+C). */
    close(listener);
    return EXIT_SUCCESS;
}
