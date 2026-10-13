/*
 * session.h — a máquina de estados de UMA ligação, do lado do servidor.
 *
 * Recebe uma linha já enquadrada (do framing) e decide a resposta. Não toca em
 * sockets — o src/server/main.c faz o recv/send à volta disto — por isso
 * testa-se sem rede, como as duas camadas de baixo.
 *
 *   socket -> framing -> [ session: protocol + estado ] -> resposta -> socket
 *
 *        NICK válido             MSG
 *   AWAITING_NICK ──────▶ READY ──────▶ CHAT ao próprio emissor (M1)
 *        │  OK                │
 *        │ MSG → ERROR 401    │ NICK → ERROR 400
 *
 * Decisões (PROTOCOL.md §4 — propostas, TO AGREE):
 *   D5  o emissor de MSG recebe o seu próprio CHAT. No M1 é o ÚNICO a
 *       recebê-lo (ainda não há lista de clientes); no M2 passa a broadcast.
 *   D6  NICK aceite -> OK.
 *   -   pedido de cliente antes do NICK -> ERROR 401 "nick required".
 *   -   NICK depois de registado -> ERROR 400 "nick already set" (mudar de
 *       nick não existe no M1).
 *   -   tipos S->C (OK, CHAT, ERROR) enviados por um cliente -> ERROR 400
 *       "unexpected type", em qualquer estado: não são "pedidos antes do
 *       NICK", nunca são válidos vindos de um cliente.
 *   -   um erro NÃO muda o estado nem fecha a ligação: um cliente com um bug
 *       numa mensagem pode continuar a mandar as seguintes. A exceção é a
 *       linha demasiado longa, que é do framing e é tratada no main.c.
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

void session_init(struct session *s);

/* Trata UMA linha (sem o '\n', `len` do framing) e preenche `*reply`.
 *
 * No M1 há sempre exatamente uma resposta por linha, e é sempre codificável:
 * proto_encode(reply) nunca falha com ela (os testes verificam-no em todos os
 * casos). */
void session_handle(struct session *s, const char *line, size_t len,
                    struct msg *reply);

#endif /* SESSION_H */
