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
 *
 * O QUE MUDOU DO M3 PARA O M4 (slide 123: "Idle timeout, clean handling of
 * abrupt disconnects and bad JSON, Ctrl+C shutdown that closes sockets,
 * logging")
 * -----------------------------------------------------------------------
 *   - Cada thread deixou de ficar num recv() sem fim: espera com prazo
 *     (wait_readable). Sem nenhuma mensagem durante IDLE (300 s) → ERROR 408
 *     "idle timeout" e fecho; quem liga e não faz NICK em NICK_WAIT (30 s) →
 *     408 "nick timeout". É isto que apanha a "morte silenciosa" (cabo
 *     arrancado, portátil sem bateria: nem FIN nem RST), que até aqui
 *     deixava uma thread e um lugar presos para sempre (typical-problems
 *     §2.3). (D23)
 *   - Fecho limpo: sempre que somos NÓS a fechar (QUIT, 408, 413,
 *     encerramento), linger_drain() — FIN depois da última resposta, e ler e
 *     deitar fora o que o cliente ainda mandar até ele fechar (máx. 2 s).
 *     Antes, fechar com bytes por ler gerava RST, e o RST podia engolir o
 *     próprio 413 ou o OK ao QUIT (M0 nota 4; probe_m3 §8 via-o às vezes).
 *   - Ctrl+C (SIGINT, SIGTERM, SIGHUP): deixa de aceitar, cada ligação recebe
 *     EVENT "server shutting down" e é fechada como acima, espera-se pelas
 *     threads e o servidor sai com 0, sem fugas (roster_destroy). O handler
 *     só escreve um byte num pipe (stop_pipe, o "self-pipe trick"); o ciclo
 *     do accept() e TODAS as threads vigiam esse pipe no seu poll(), e
 *     acordam. (D24)
 *   - Logging (log.h): hora, nível, uma linha por evento, e a CAUSA de cada
 *     saída ("QUIT", "FIN", "RST", "inativo ha 300 s (408)", ...).
 *
 * Uso: ./bin/server [-v | -q] [-i segundos] [-n segundos] [porta]
 *   -v  log com DEBUG (cada mensagem recebida)    -q  só WARN e ERROR
 *   -i  inatividade até ao 408 (por omissão 300)  -n  prazo para o NICK (30)
 */
#include "framing.h"
#include "log.h"
#include "net.h"
#include "protocol.h"
#include "roster.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   /* strerror */
#include <poll.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT    "5000"

/* Quanto tempo um cliente pode estar sem ler antes de ser desligado. Tem de
 * ser muito maior do que qualquer atraso normal (um portátil em Wi-Fi fraco)
 * e curto o suficiente para o resto do chat não dar pela pausa como avaria. */
#define SEND_TIMEOUT_MS 5000

/* M4 (D23): por omissão; -i e -n mudam-nos (os testes usam 1-2 s). 300 s é o
 * valor do grupo 1. Antes do NICK é muito mais curto: quem ainda não entrou
 * não tem razão nenhuma para estar calado, e cada ligação ocupa um dos 64
 * lugares. */
#define IDLE_S_DEFAULT      300
#define NICK_WAIT_S_DEFAULT 30

/* M4: quanto se espera, depois do nosso FIN, que o cliente feche também. No
 * encerramento é mais curto: não se quer o Ctrl+C a demorar. */
#define LINGER_MS      2000
#define STOP_LINGER_MS 1000

#define SRV_R_FULL "server full"   /* ERROR 503, PROTOCOL.md §5 */

/* A lista de clientes é global porque é o que as threads partilham. Toda a
 * concorrência que lhe diz respeito está dentro do roster.c. */
static struct roster R;

static long idle_ms      = IDLE_S_DEFAULT * 1000L;
static long nick_wait_ms = NICK_WAIT_S_DEFAULT * 1000L;

/* M4: o encerramento. O handler só pode mexer em variáveis destas (e num
 * _Atomic sem lock, e no write() do pipe, que é async-signal-safe). */
static volatile sig_atomic_t got_signal = 0;
static _Atomic int           stopping   = 0;   /* lido pelas threads */
static int                   stop_pipe[2] = { -1, -1 };

/* M4: quantas threads de cliente estão vivas — para o main() saber quando
 * pode chamar roster_destroy() (com threads vivas seria um use-after-free). */
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  threads_cond;
static int             nthreads = 0;

static void thread_started(void)
{
    pthread_mutex_lock(&threads_lock);
    nthreads++;
    pthread_mutex_unlock(&threads_lock);
}

static void thread_done(void)
{
    pthread_mutex_lock(&threads_lock);
    if (--nthreads == 0)
        pthread_cond_broadcast(&threads_cond);
    pthread_mutex_unlock(&threads_lock);
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

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
 *     shutdown() só acorda a thread dona (no wait_readable/recv), que faz a
 *     saída normal — roster_remove() e o EVENT "x left" para os outros.
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
            log_warn("%s nao le ha %d ms: a desligar", c->who, SEND_TIMEOUT_MS);
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
 * (send_line_to), e a thread DONO desse cliente dá por isso e trata da saída.
 * Assim um cliente morto nunca faz parar a thread de outro. */
static int deliver(struct client *from, const struct sess_out *o)
{
    char   line[PROTO_LINE_BUF];
    size_t len;

    /* Codificar uma vez só, antes de qualquer envio. */
    if (proto_encode(&o->msg, line, sizeof line, &len) != 0) {
        /* Inalcançável: a session só produz mensagens válidas e os testes
         * verificam-no caso a caso. Se acontecer, é um bug NOSSO. */
        log_error("%s: BUG: %s nao codificavel", from->who, proto_type_name(o->msg.type));
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
            log_info("%s: PRIV para %s, que entretanto saiu", from->who, o->msg.to);
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

/* Como acabou o serve(): diz ao client_thread() se ainda tem de esperar pelo
 * fecho do cliente (M4). */
enum serve_end {
    END_PEER,   /* foi o cliente (FIN, RST, erro): não há nada a esperar      */
    END_OURS,   /* fomos nós (QUIT, 408, 413, encerramento): linger_drain()   */
    END_DEAD    /* o envio ao próprio falhou: já houve shutdown(), mais nada  */
};

/* Serve uma ligação até ela fechar, e escreve em why[] PORQUÊ (para o log).
 * Os buffers são locais da thread: ~300 KB no pior caso, numa stack de 8 MB,
 * e assim cada cliente tem os seus sem alocação nenhuma. */
static enum serve_end serve(struct client *c, struct session *sess,
                            char *why, size_t why_cap)
{
    struct rxbuf    rx;
    char            chunk[RX_CHUNK];
    char            line[RX_LINE_BUF];
    struct sess_out outs[SESS_MAX_OUT];
    /* Local da thread, logo sem lock: só esta thread muda o estado desta
     * sessão. Evita uma chamada com lock por cada mensagem recebida. */
    int             joined = 0;

    /* D23: antes do NICK o prazo conta desde a ligação (não é renovado por
     * linhas recusadas); depois, desde a última mensagem. */
    long deadline = now_ms() + nick_wait_ms;

    rxbuf_init(&rx);

    for (;;) {
        /* 1. Esperar por bytes, mas não para sempre (M4). */
        long left = deadline - now_ms();
        int  w    = left > 0 ? wait_readable(c->fd, stop_pipe[0], left) : NET_TIMEOUT;

        if (w == NET_STOP) {                /* Ctrl+C no servidor (D24) */
            struct sess_out o;
            session_shutdown(&o);
            deliver(c, &o);
            snprintf(why, why_cap, "servidor a encerrar");
            return END_OURS;
        }
        if (w == NET_TIMEOUT) {             /* inativo de mais (D23) */
            struct sess_out o;
            session_timeout(sess, &o);
            if (sess->state == SESS_READY)
                snprintf(why, why_cap, "inativo ha %ld s (408)", idle_ms / 1000);
            else
                snprintf(why, why_cap, "sem NICK em %ld s (408)", nick_wait_ms / 1000);
            return deliver(c, &o) == 0 ? END_OURS : END_DEAD;
        }
        if (w < 0) {
            snprintf(why, why_cap, "erro no poll: %s", strerror(errno));
            return END_PEER;
        }

        /* 2. Nunca mais do que RX_CHUNK por recv(): é essa a disciplina que
         *    garante que o rxbuf não transborda (framing.md §3.1). */
        ssize_t n = recv_retry(c->fd, chunk, sizeof chunk);
        if (n == 0) {
            /* Se foi outra thread a desligá-lo (send_line_to: não lia), o
             * recv() também dá 0 — dizer a causa verdadeira. */
            snprintf(why, why_cap, "%s", atomic_load(&c->dead)
                     ? "deixou de ler (prazo de envio)" : "fechou a ligacao (FIN)");
            return END_PEER;
        }
        if (n < 0) {
            /* ECONNRESET é o cliente a desaparecer à bruta (kill -9, cabo
             * arrancado com a pilha TCP ainda viva), não um erro nosso. */
            snprintf(why, why_cap, "%s", errno == ECONNRESET
                     ? "ligacao abortada (RST)" : strerror(errno));
            return END_PEER;
        }

        if (rxbuf_feed(&rx, chunk, (size_t)n) != 0) {
            log_error("%s: BUG: rxbuf_feed falhou", c->who);
            snprintf(why, why_cap, "BUG no framing");
            return END_PEER;
        }

        /* 3. Um recv() pode trazer zero, uma ou VÁRIAS mensagens: ciclo, não
         *    if. E três saídas, não duas (framing.h). */
        for (;;) {
            size_t len;
            int r = rxbuf_next_line(&rx, line, sizeof line, &len);

            if (r == RX_NEED)                   /* faltam bytes: voltar a esperar */
                break;

            if (r == RX_TOOLONG) {              /* culpa DELES (D1/D2) */
                /* 413 e não 400 (revisão do M3, acordado com o primeiro
                 * grupo): 413 = "a ligação não está em condições e é
                 * fechada"; 400 = "pedido recusado, continua". */
                struct sess_out e = { .dest = SESS_SELF };
                e.msg.type = MT_ERROR;
                e.msg.code = 413;
                snprintf(e.msg.reason, sizeof e.msg.reason, "%s", SESS_R_TOOLONG);
                snprintf(why, why_cap, "linha com mais de %d bytes (413)", RX_MAX_LINE);
                /* Fechar sem ressincronizar (D2). O resto da linha ainda está
                 * a chegar: o linger_drain() lê-o e deita-o fora, para o
                 * fecho sair como FIN e o 413 chegar (M4). */
                return deliver(c, &e) == 0 ? END_OURS : END_DEAD;
            }

            if (r != RX_LINE) {                 /* RX_FAULT: culpa NOSSA */
                log_error("%s: BUG: rxbuf_next_line = %d", c->who, r);
                snprintf(why, why_cap, "BUG no framing");
                return END_PEER;
            }

            if (log_enabled(LV_DEBUG))
                log_debug("%s <- %.*s%s", c->who, len > 200 ? 200 : (int)len, line,
                          len > 200 ? "..." : "");

            size_t nout = session_handle(sess, line, len, outs, SESS_MAX_OUT);

            /* D23: uma mensagem recebida renova o prazo — mas só depois do
             * NICK. Antes, o prazo é fixo desde a ligação: linhas recusadas
             * não chegam para ficar com um lugar. */
            if (sess->state == SESS_READY)
                deadline = now_ms() + idle_ms;

            for (size_t i = 0; i < nout; i++) {
                if (outs[i].msg.type == MT_ERROR)
                    log_info("%s -> ERROR %d (%s)",
                             c->who, outs[i].msg.code, outs[i].msg.reason);
                if (deliver(c, &outs[i]) != 0) {
                    snprintf(why, why_cap, "o envio falhou");
                    return END_DEAD;            /* só falha no envio ao próprio */
                }

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
                    log_info("%s entrou como %s (%zu no chat)", c->who, sess->nick,
                             roster_count(&R));
                }
            }

            /* QUIT (D17): o OK já seguiu. Parar de ler e sair como numa
             * queda — o client_thread() faz o roster_remove() e o "x left".
             * O que vier depois do QUIT, mesmo que já esteja no rxbuf, é
             * ignorado; o que ainda estiver a chegar é lido e deitado fora
             * pelo linger_drain() (M4), para o fecho sair como FIN e não
             * como RST (que podia engolir o próprio OK). */
            if (sess->state == SESS_CLOSED) {
                snprintf(why, why_cap, "QUIT");
                return END_OURS;
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
    char            why[96] = "?";

    session_init(&sess, &dir);
    enum serve_end end = serve(c, &sess, why, sizeof why);

    /* Tirar da lista ANTES de anunciar a saída: assim o retrato do EVENT já
     * não o inclui, e não há hipótese de alguém receber uma mensagem de um
     * cliente que já se foi embora. */
    roster_remove(&R, c);

    /* No encerramento (D24) ninguém é avisado de que os outros saem: estão
     * todos a sair, e o aviso já foi o "server shutting down". */
    struct msg ev;
    int        stop = atomic_load(&stopping);
    if (session_leave(&sess, &ev) && !stop) {   /* 0 se saiu antes do NICK */
        struct sess_out o = { .dest = SESS_OTHERS, .msg = ev };
        deliver(c, &o);
    }
    if (sess.nick[0] != '\0')
        log_info("%s (%s) saiu: %s (%zu no chat)", c->who, sess.nick, why, roster_count(&R));
    else
        log_info("%s saiu sem nick: %s", c->who, why);

    /* M4: fomos nós a fechar → FIN depois da última resposta, e esperar que
     * o cliente feche também (lendo o que ainda mandar). Só então o close()
     * (no client_release) sai como FIN e não como RST. */
    if (end == END_OURS) {
        int r = linger_drain(c->fd, stop ? STOP_LINGER_MS : LINGER_MS);
        if (r == 1)
            log_debug("%s nao fechou a tempo: fecho forcado", c->who);
    }

    /* A última referência desta thread. Fecha o fd e liberta — a não ser que
     * um broadcast ainda tenha o cliente no retrato, caso em que só desaparece
     * quando esse largar a referência. */
    client_release(&R, c);
    thread_done();
    return NULL;
}

/* ---- Arranque e encerramento (M4) --------------------------------------- */

/* O "self-pipe trick": o handler não faz nada a não ser marcar e escrever
 * um byte no pipe. Quem estiver num poll() a vigiar o pipe acorda: o ciclo do
 * accept() e todas as threads de cliente (wait_readable). Ninguém lê o byte,
 * por isso o pipe fica legível para sempre — as threads que só chegarem ao
 * poll() mais tarde também o veem.
 *
 * (A 1.ª versão usava pselect() com os sinais bloqueados fora dele. Funciona,
 * mas sob o ThreadSanitizer o sinal só era tratado na chamada seguinte que
 * ele interceta — o accept() da ligação seguinte —, e o Ctrl+C ficava à
 * espera de um cliente. Com um pipe, quem acorda é um fd, não um EINTR.) */
static void on_stop(int sig)
{
    int e = errno;
    if (!got_signal) {
        got_signal = sig;
        atomic_store(&stopping, 1);
        if (write(stop_pipe[1], "x", 1) < 0) { /* nada a fazer num handler */ }
    }
    errno = e;
}

static const char *signame(int sig)
{
    return sig == SIGINT ? "SIGINT (Ctrl+C)" : sig == SIGTERM ? "SIGTERM"
         : sig == SIGHUP ? "SIGHUP" : "sinal";
}

static void usage(const char *prog)
{
    fprintf(stderr, "uso: %s [-v | -q] [-i segundos] [-n segundos] [porta]\n"
                    "  -v  log com DEBUG       -q  so WARN e ERROR\n"
                    "  -i  inatividade ate ao ERROR 408 (por omissao %d)\n"
                    "  -n  prazo para o NICK (por omissao %d)\n",
            prog, IDLE_S_DEFAULT, NICK_WAIT_S_DEFAULT);
}

/* "-i 2": um inteiro positivo de segundos, ou -1. */
static long parse_secs(const char *s)
{
    char *end;
    long  v = strtol(s, &end, 10);
    return (*s != '\0' && *end == '\0' && v > 0 && v <= 86400) ? v : -1;
}

/* Encerramento (D24): parar de aceitar, acordar todas as threads (cada uma
 * avisa o seu cliente e fecha), esperar por elas e libertar tudo. */
static void shutdown_server(int listener)
{
    close(listener);                        /* ninguém novo entra */
    log_info("%s: a encerrar (%zu ligacao(oes) abertas)", signame((int)got_signal),
             roster_count(&R));

    /* O handler já escreveu no pipe e marcou `stopping`: as threads já estão
     * a avisar os seus clientes e a fechar. Falta esperar por elas. */

    /* O pior caso de uma thread: um envio a meio (5 s) + o linger (1 s). */
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += (SEND_TIMEOUT_MS + STOP_LINGER_MS) / 1000 + 1;

    pthread_mutex_lock(&threads_lock);
    while (nthreads > 0)
        if (pthread_cond_timedwait(&threads_cond, &threads_lock, &until) == ETIMEDOUT)
            break;
    int left = nthreads;
    pthread_mutex_unlock(&threads_lock);

    if (left == 0) {
        /* Só agora: com threads vivas seria um use-after-free (roster.h). */
        roster_destroy(&R);
        log_info("encerrado: todas as ligacoes fechadas");
    } else {
        log_warn("%d ligacao(oes) nao fecharam a tempo: a sair na mesma", left);
    }
    close(stop_pipe[0]);
    close(stop_pipe[1]);
}

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "vqi:n:h")) != -1) {
        switch (opt) {
        case 'v': log_set_level(LV_DEBUG); break;
        case 'q': log_set_level(LV_WARN);  break;
        case 'i':
        case 'n': {
            long v = parse_secs(optarg);
            if (v < 0) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (opt == 'i')
                idle_ms = v * 1000;
            else
                nick_wait_ms = v * 1000;
            break;
        }
        default:
            usage(argv[0]);
            return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    const char *port = (optind < argc) ? argv[optind] : DEFAULT_PORT;

    roster_init(&R);

    /* A condvar mede o prazo no relógio MONOTONIC (o da hora do sistema
     * pode saltar). */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&threads_cond, &ca);
    pthread_condattr_destroy(&ca);

    if (pipe(stop_pipe) != 0) {
        log_error("pipe: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    /* Sinais de encerramento (M4). Sem SA_RESTART não é preciso (ninguém
     * espera pelo EINTR: acorda-se pelo pipe), por isso SA_RESTART: assim um
     * Ctrl+C não interrompe um fwrite() do log a meio. E as threads de
     * cliente nascem com estes sinais BLOQUEADOS (ver o pthread_create): o
     * handler corre sempre na thread principal. */
    sigset_t stopset;
    sigemptyset(&stopset);
    sigaddset(&stopset, SIGINT);
    sigaddset(&stopset, SIGTERM);
    sigaddset(&stopset, SIGHUP);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    int listener = tcp_listen(port, 8);
    if (listener < 0)
        return EXIT_FAILURE;
    /* Não bloqueante: o poll() diz "há uma ligação", mas ela pode desistir
     * antes do accept() — e um accept() bloqueante ficava então preso até à
     * ligação seguinte, com o Ctrl+C por atender. */
    fcntl(listener, F_SETFL, fcntl(listener, F_GETFL) | O_NONBLOCK);

    log_info("a escuta na porta %s (max %d clientes; inatividade %ld s, NICK em %ld s)",
             port, ROSTER_MAX, idle_ms / 1000, nick_wait_ms / 1000);

    for (;;) {
        /* Esperar por uma ligação OU pelo pipe de paragem (Ctrl+C). */
        int w = wait_readable(listener, stop_pipe[0], -1);
        if (w == NET_STOP)
            break;
        if (w < 0) {
            log_error("poll: %s", strerror(errno));
            break;
        }

        struct sockaddr_storage peer;          /* cabe IPv4 ou IPv6 */
        socklen_t peer_len = sizeof peer;

        /* accept() devolve um socket NOVO, só para este cliente (bloqueante:
         * o O_NONBLOCK do listener não passa para ele em Linux; e o
         * wait_readable/send_all_timeout não dependem disso). */
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            /* Um accept() falhado é quase sempre problema DESSA ligação
             * (ECONNABORTED: desistiu antes de ser aceite) ou passageiro
             * (EMFILE/ENFILE: sem fds livres, ENOBUFS/ENOMEM). Nenhum é razão
             * para deitar abaixo o chat de toda a gente. */
            if (errno == EINTR || errno == ECONNABORTED ||
                errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            log_error("accept: %s", strerror(errno));
            if (errno == EMFILE || errno == ENFILE ||
                errno == ENOBUFS || errno == ENOMEM) {
                sleep(1);                       /* dar tempo a que saia alguém */
                continue;
            }
            break;                              /* EBADF, EINVAL...: bug nosso */
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);  /* por via das dúvidas */

        char who[64];
        addr_to_str((struct sockaddr *)&peer, who, sizeof who);

        struct client *c = roster_add(&R, fd, who);
        if (c == NULL) {
            /* Lista cheia ou sem memória: recusar em condições, com um ERROR
             * que o cliente consegue mostrar, em vez de um fecho sem
             * explicação (PROTOCOL.md §5). Prazos curtos: não vale a pena
             * prender o ciclo do accept() por causa de quem vai ser recusado
             * (e o linger evita que um RST engula o 503). */
            log_warn("%s recusado: lista cheia (503)", who);
            struct msg e = { .type = MT_ERROR, .code = 503 };
            snprintf(e.reason, sizeof e.reason, "%s", SRV_R_FULL);
            char   line[PROTO_LINE_BUF];
            size_t len;
            if (proto_encode(&e, line, sizeof line, &len) == 0)
                send_all_timeout(fd, line, len, 200);
            linger_drain(fd, 200);
            close(fd);
            continue;
        }

        pthread_t tid;
        /* Detached: ninguém faz join (o encerramento espera pelo contador),
         * e sem isto os recursos de cada thread nunca seriam libertados. */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        thread_started();
        sigset_t before;                        /* a thread herda a máscara: */
        pthread_sigmask(SIG_BLOCK, &stopset, &before);   /* sinais bloqueados */
        int rc = pthread_create(&tid, &attr, client_thread, c);
        pthread_sigmask(SIG_SETMASK, &before, NULL);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            log_error("pthread_create: %s", strerror(rc));
            thread_done();
            roster_remove(&R, c);
            client_release(&R, c);              /* fecha o fd */
            continue;
        }

        log_info("%s ligou (%zu ligacao(oes))", who, roster_count(&R));
    }

    /* Um segundo Ctrl+C durante o encerramento mata o processo logo (ação
     * por omissão), para quem não quiser esperar. */
    signal(SIGINT, SIG_DFL);

    if (!got_signal) {
        /* Só por um bug nosso (poll/accept com EBADF/EINVAL): encerrar como
         * no Ctrl+C, acordando as threads pelo mesmo pipe. */
        log_error("o ciclo do accept() parou sem sinal: a encerrar");
        on_stop(SIGTERM);
    }
    shutdown_server(listener);
    return EXIT_SUCCESS;
}
