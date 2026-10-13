/*
 * session.c — máquina de estados de uma ligação. Contrato em include/session.h.
 */
#include "session.h"

#include <stdio.h>
#include <string.h>

/* Um nick cabe sempre num text com folga: 32 + strlen(" joined") = 39, muito
 * abaixo de 1024. Se os limites mudarem, isto deixa de compilar em vez de
 * truncar o EVENT em silêncio. */
_Static_assert(PROTO_NICK_MAX + sizeof(" joined") <= PROTO_TEXT_MAX,
               "o texto de um EVENT tem de caber num text");

void session_init(struct session *s)
{
    memset(s, 0, sizeof *s);                /* AWAITING_NICK, nick "" */
}

/* Preenche out[0] com um ERROR para quem enviou a linha. Devolve 1 (o número
 * de saídas), para o chamador poder fazer `return only_error(...)`. */
static size_t only_error(struct sess_out *out, int code, const char *reason)
{
    memset(out, 0, sizeof *out);
    out->dest     = SESS_SELF;              /* um erro é de quem se enganou */
    out->msg.type = MT_ERROR;
    out->msg.code = code;
    /* reason é sempre um dos literais do protocol.h/session.h, todos bem
     * abaixo de PROTO_REASON_MAX; o strncpy + '\0' é só cinto de segurança. */
    strncpy(out->msg.reason, reason, sizeof out->msg.reason - 1);
    return 1;
}

/* Constrói um EVENT "<nick> <o quê>" (slide 124: {"type":"EVENT",
 * "text":"rui joined"}). O nick já passou pelo proto_valid_nick, logo não tem
 * espaços nem caracteres de controlo e o texto resultante é sempre válido. */
static void fill_event(struct msg *m, const char *nick, const char *what)
{
    memset(m, 0, sizeof *m);
    m->type = MT_EVENT;
    snprintf(m->text, sizeof m->text, "%s %s", nick, what);
}

size_t session_handle(struct session *s, const char *line, size_t len,
                      struct sess_out *out, size_t cap)
{
    struct msg       in;
    struct proto_err err;

    if (cap == 0)
        return 0;

    /* 1. A linha é uma mensagem válida? Se não, o protocolo já diz porquê. */
    if (proto_parse(line, len, &in, &err) != 0)
        return only_error(out, err.code, err.reason);

    switch (in.type) {
    case MT_NICK:
        if (s->state == SESS_READY)
            return only_error(out, 400, SESS_R_NICK_SET);

        /* in.nick já foi validado pelo protocolo e tem o mesmo tamanho. */
        memcpy(s->nick, in.nick, sizeof s->nick);
        s->state = SESS_READY;

        /* Duas saídas, e a ordem importa: o OK vai primeiro para que quem
         * entrou saiba que foi aceite antes de começar a receber tráfego. */
        memset(&out[0], 0, sizeof out[0]);
        out[0].dest     = SESS_SELF;
        out[0].msg.type = MT_OK;                                /* D6 */
        if (cap < 2)
            return 1;

        out[1].dest = SESS_OTHERS;   /* quem entra não precisa de ser avisado */
        fill_event(&out[1].msg, s->nick, "joined");
        return 2;

    case MT_MSG:
        if (s->state != SESS_READY)
            return only_error(out, 401, SESS_R_NICK_REQUIRED);  /* #17 */

        memset(&out[0], 0, sizeof out[0]);
        /* SESS_ALL e não SESS_OTHERS: o emissor recebe o seu próprio CHAT
         * (D5). É isso que lhe confirma que a mensagem chegou ao servidor, e é
         * por isso que o cliente não deve ecoar localmente o que escreve. */
        out[0].dest             = SESS_ALL;
        out[0].msg.type         = MT_CHAT;
        out[0].msg.is_private   = 0;
        /* O `from` vem da SESSÃO, nunca do que o cliente mandou: senão
         * qualquer um se fazia passar por outro. */
        memcpy(out[0].msg.from, s->nick, sizeof out[0].msg.from);
        memcpy(out[0].msg.text, in.text, sizeof out[0].msg.text);
        return 1;

    default:
        /* OK, CHAT, ERROR, EVENT: válidos no protocolo, mas só no sentido
         * S->C. Um cliente que os envie está enganado, em qualquer estado. */
        return only_error(out, 400, SESS_R_UNEXPECTED);
    }
}

int session_leave(const struct session *s, struct msg *out)
{
    /* Saiu antes de escolher nick: ninguém sabia que estava cá, não há nada a
     * anunciar. Acontece sempre que alguém liga e desliga sem falar — um
     * scanner de portas, por exemplo. */
    if (s->state != SESS_READY)
        return 0;

    fill_event(out, s->nick, "left");
    return 1;
}
