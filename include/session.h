/*
 * session.h — a máquina de estados de UMA ligação, do lado do servidor.
 *
 * Recebe uma linha já enquadrada (do framing) e decide o que sai. Não toca em
 * sockets nem no roster — o src/server/main.c faz o recv/send e o encaminhamento
 * à volta disto — por isso testa-se sem rede, como as duas camadas de baixo.
 *
 *   socket -> framing -> [ session: protocol + estado ] -> saídas -> socket(s)
 *
 *        NICK válido             MSG
 *   AWAITING_NICK ──────▶ READY ──────▶ CHAT a toda a gente (M2)
 *        │  OK + EVENT        │
 *        │ MSG → ERROR 401    │ NICK → ERROR 400
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
 *       escreve: o eco vem do servidor, e serve de confirmação de que a
 *       mensagem lá chegou mesmo.
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
 */
#ifndef SESSION_H
#define SESSION_H

#include <stddef.h>

#include "protocol.h"

#define SESS_R_NICK_REQUIRED "nick required"
#define SESS_R_NICK_SET      "nick already set"
#define SESS_R_UNEXPECTED    "unexpected type"
#define SESS_R_TOOLONG       "line too long"     /* usado pelo main.c */

enum sess_state {
    SESS_AWAITING_NICK = 0,   /* 0 de propósito: uma session a zeros é o início */
    SESS_READY
};

struct session {
    enum sess_state state;
    char nick[PROTO_NICK_MAX + 1];   /* só tem significado em SESS_READY */
};

/* Para quem vai uma saída. O main.c traduz isto em fds através do roster. */
enum sess_dest {
    SESS_SELF = 0,   /* só a quem enviou a linha                            */
    SESS_ALL,        /* todos os ligados, INCLUINDO o emissor (D5)          */
    SESS_OTHERS      /* todos menos o emissor (os EVENT de entrada e saída) */
};

struct sess_out {
    enum sess_dest dest;
    struct msg     msg;
};

/* Máximo de saídas que uma linha pode produzir. São 2: o NICK aceite dá OK +
 * EVENT. Se um dia passar a 3, o _Static_assert no .c obriga a reparar nisso
 * em vez de truncar em silêncio. */
#define SESS_MAX_OUT 2

void session_init(struct session *s);

/* Trata UMA linha (sem o '\n', `len` do framing) e escreve as saídas em out[].
 *
 * Devolve quantas saídas produziu (1 ou 2; nunca 0 no M2 — toda a linha tem
 * resposta, nem que seja um ERROR). `cap` deve ser >= SESS_MAX_OUT; com menos,
 * devolve só as que couberem.
 *
 * Todas as saídas são codificáveis: proto_encode() nunca falha com elas (os
 * testes verificam-no em todos os casos). */
size_t session_handle(struct session *s, const char *line, size_t len,
                      struct sess_out *out, size_t cap);

/* O EVENT de saída, para quando a ligação fecha.
 *
 * Não vem de nenhuma linha — vem do fim do ciclo de recv() —, por isso é uma
 * função à parte. Devolve 1 e preenche *out se houver algo a anunciar, ou 0 se
 * o cliente se foi embora ANTES de escolher nick: ninguém sabia que ele estava
 * lá, por isso não faz sentido avisar que saiu.
 *
 * O destino é sempre SESS_OTHERS (quem sai já não está para receber). */
int session_leave(const struct session *s, struct msg *out);

#endif /* SESSION_H */
