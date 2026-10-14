/*
 * session.h — a máquina de estados de UMA ligação, do lado do servidor.
 *
 * Recebe uma linha já enquadrada (do framing) e decide o que sai. Não toca em
 * sockets nem no roster — o src/server/main.c faz o recv/send e o encaminhamento
 * à volta disto — por isso testa-se sem rede, como as duas camadas de baixo.
 *
 *   socket -> framing -> [ session: protocol + estado ] -> saídas -> socket(s)
 *
 *        NICK válido e livre         MSG  → CHAT a todos (M2)
 *   AWAITING_NICK ──────────▶ READY  PRIV → CHAT privado a um (M3)
 *     │  OK + EVENT             │    LIST → USERS (M3)
 *     │                         │    NICK → ERROR 400
 *     │ MSG/PRIV/LIST → 401     │
 *     │ NICK em uso   → 409     │ QUIT → OK
 *     │ QUIT → OK               ▼
 *     └──────────────────────▶ CLOSED   (o main.c fecha a ligação)
 *
 *
 * O QUE MUDOU DO M1 PARA O M2
 * ---------------------------
 * No M1 havia sempre exatamente UMA resposta, e era sempre para quem enviou a
 * linha. No M2 isso deixa de chegar por duas razões:
 *
 *   - uma linha pode produzir DUAS saídas: um NICK aceite dá "OK" a quem o
 *     mandou E um EVENT a toda a gente a avisar que entrou;
 *   - o destino deixa de ser óbvio: um CHAT vai para todos, um ERROR só para
 *     quem se enganou.
 *
 * Daí `struct sess_out`, que junta a mensagem ao destino. A session continua
 * sem saber que o roster existe: limita-se a dizer PARA QUEM, e é o main.c que
 * resolve o "quem" consultando o roster. É isso que a mantém testável sem rede.
 *
 *
 * DECISÕES (PROTOCOL.md §4)
 * -------------------------
 *   D5  o emissor de MSG recebe o seu próprio CHAT -> SESS_ALL, não
 *       SESS_OTHERS. É por isso que o cliente NÃO deve ecoar localmente o que
 *       escreve para todos: o eco vem do servidor. (O PRIV é diferente desde
 *       a revisão do M3: ver D14.)
 *   D6  NICK aceite -> OK.
 *   -   pedido de cliente antes do NICK -> ERROR 401 "nick required".
 *   -   NICK depois de registado -> ERROR 400 "nick already set" (mudar de
 *       nick não existe no M2).
 *   -   tipos S->C (OK, CHAT, ERROR, EVENT) enviados por um cliente -> ERROR
 *       400 "unexpected type", em qualquer estado: não são "pedidos antes do
 *       NICK", nunca são válidos vindos de um cliente.
 *   -   um erro NÃO muda o estado nem fecha a ligação: um cliente com um bug
 *       numa mensagem pode continuar a mandar as seguintes. A exceção é a
 *       linha demasiado longa, que é do framing e é tratada no main.c.
 *
 * TO AGREE com os outros grupos: o texto exato dos EVENT ("ana joined" /
 * "ana left", do slide 124) e quem os recebe (aqui: todos menos o próprio —
 * quem entra não precisa de ser avisado de que entrou).
 *
 *
 * O QUE MUDOU DO M2 PARA O M3
 * ---------------------------
 * Três dos pedidos novos precisam de saber quem MAIS está ligado: um NICK
 * tem de ser único (409), um PRIV tem de ter destino (404), e o LIST
 * devolve a lista. Isso está no roster — que a session não pode conhecer, ou
 * deixava de se testar sem rede e sem threads.
 *
 * Daí `struct sess_dir`, um "diretório": três funções que a session chama
 * sem saber o que está por trás. No servidor é o roster (main.c); nos testes
 * é um array (test_session.c). As DECISÕES continuam todas aqui — o diretório
 * só responde a perguntas ("está livre?", "existe?", "quem está?").
 *
 * DECISÕES DO M3 (PROTOCOL.md §4). D13 e D14 foram REVISTAS com o primeiro
 * grupo com quem falámos (M3.md, "Revisão do M3"):
 *   D13 "OK = last request accepted" (slide 124), à letra: NICK, MSG, PRIV e
 *       QUIT aceites recebem OK, e o OK sai SEMPRE antes do CHAT. O LIST é
 *       respondido pelo USERS (slide: "Reply to LIST"). Um pedido recusado
 *       recebe só o ERROR.
 *       (Antes da revisão: MSG e PRIV eram confirmados pelo CHAT, sem OK.)
 *   D14 PRIV: só o destino recebe o CHAT (private:true). O emissor recebe o
 *       OK e NÃO recebe cópia: um CHAT é "uma mensagem que te chegou", e uma
 *       cópia mudava esse significado. Quem quiser ver o que enviou mostra-o
 *       localmente (o nosso cliente faz isso). A si próprio: OK + o CHAT.
 *       (Antes da revisão: cópia ao emissor, com um campo "to" no CHAT.)
 *   D15 PRIV para um nick que não está no chat → ERROR 404 "no such nick".
 *   D16 USERS: quem está no chat, INCLUINDO o próprio, por ordem alfabética
 *       (proto_nick_cmp). Quem ainda não fez NICK não aparece.
 *   D17 QUIT → OK e a ligação fecha (estado CLOSED); os outros recebem
 *       "<nick> left". Funciona também ANTES do NICK (sair é sempre possível).
 *   D18 "o mesmo nick" = proto_nick_eq() (sem olhar a maiúsculas ASCII).
 *   D19 NICK em uso → ERROR 409 "nick taken"; o estado NÃO muda: o cliente
 *       pode tentar outro na mesma ligação.
 *   -   PRIV e LIST antes do NICK → ERROR 401, como o MSG.
 *   -   USERS vindo de um cliente → ERROR 400 "unexpected type" (é S → C).
 */
#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>

#include "protocol.h"

#define SESS_R_NICK_REQUIRED "nick required"
#define SESS_R_NICK_SET      "nick already set"
#define SESS_R_UNEXPECTED    "unexpected type"
#define SESS_R_TOOLONG       "line too long"     /* usado pelo main.c */
#define SESS_R_NICK_TAKEN    "nick taken"        /* 409, o texto do slide 124 */
#define SESS_R_NO_SUCH_NICK  "no such nick"      /* 404 */

enum sess_state {
    SESS_AWAITING_NICK = 0,   /* 0 de propósito: uma session a zeros é o início */
    SESS_READY,
    SESS_CLOSED               /* depois de um QUIT: o main.c fecha a ligação */
};

/* O "diretório": o que a session precisa de perguntar sobre os OUTROS.
 * Todas as funções recebem `ctx` tal e qual. Nenhuma pode ser NULL. */
struct sess_dir {
    void *ctx;

    /* Reserva `nick` para ESTA ligação se mais ninguém o tiver (D18).
     * 1 = reservado, 0 = em uso. Tem de ser atómico (roster_claim). */
    int (*claim)(void *ctx, const char *nick);

    /* Há alguém NO CHAT com este nick? 1 e o nick tal como foi registado em
     * canon; ou 0. */
    int (*lookup)(void *ctx, const char *nick, char canon[PROTO_NICK_MAX + 1]);

    /* Os nicks de quem está no chat (incluindo esta ligação, se já entrou),
     * por qualquer ordem. Devolve quantos (no máximo cap). */
    size_t (*nicks)(void *ctx, char (*out)[PROTO_NICK_MAX + 1], size_t cap);
};

struct session {
    enum sess_state        state;
    char                   nick[PROTO_NICK_MAX + 1]; /* "" até um NICK ser aceite */
    const struct sess_dir *dir;
};

/* Para quem vai uma saída. O main.c traduz isto em fds através do roster. */
enum sess_dest {
    SESS_SELF = 0,   /* só a quem enviou a linha                            */
    SESS_ALL,        /* todos os ligados, INCLUINDO o emissor (D5)          */
    SESS_OTHERS,     /* todos menos o emissor (os EVENT de entrada e saída) */
    SESS_TO          /* só quem tem o nick msg.to (PRIV, M3)                */
};

struct sess_out {
    enum sess_dest dest;
    struct msg     msg;
};

/* Máximo de saídas que uma linha pode produzir. São 2, e o OK é sempre a
 * primeira: NICK -> OK + EVENT; MSG -> OK + CHAT a todos; PRIV -> OK + CHAT
 * ao destino. */
#define SESS_MAX_OUT 2

/* Começa em AWAITING_NICK, com o diretório desta ligação. */
void session_init(struct session *s, const struct sess_dir *dir);

/* Trata UMA linha (sem o '\n', `len` do framing) e escreve as saídas em out[].
 *
 * Devolve quantas saídas produziu (1 ou 2 — toda a linha tem resposta, nem
 * que seja um ERROR). `cap` deve ser >= SESS_MAX_OUT; com menos, devolve só
 * as que couberem. Em SESS_CLOSED devolve 0: depois do QUIT não se responde a
 * mais nada (o main.c já não devia ter chamado).
 *
 * Depois de chamar, se s->state == SESS_CLOSED, o main.c envia as saídas e
 * fecha a ligação (QUIT, D17).
 *
 * Todas as saídas são codificáveis: proto_encode() nunca falha com elas (os
 * testes verificam-no em todos os casos). */
size_t session_handle(struct session *s, const char *line, size_t len,
                      struct sess_out *out, size_t cap);

/* O EVENT de saída, para quando a ligação fecha.
 *
 * Não vem de nenhuma linha — vem do fim do ciclo de recv(), ou do QUIT —, por
 * isso é uma função à parte. Um QUIT e uma queda dão o mesmo "<nick> left". Devolve 1 e preenche *out se houver algo a anunciar, ou 0 se
 * o cliente se foi embora ANTES de escolher nick: ninguém sabia que ele estava
 * lá, por isso não faz sentido avisar que saiu.
 *
 * O destino é sempre SESS_OTHERS (quem sai já não está para receber). */
int session_leave(const struct session *s, struct msg *out);

#endif /* SESSION_H */
