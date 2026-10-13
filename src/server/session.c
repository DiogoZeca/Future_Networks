/*
 * session.c — máquina de estados de uma ligação. Contrato em include/session.h.
 */
#include "session.h"

#include <string.h>

void session_init(struct session *s)
{
    memset(s, 0, sizeof *s);                /* AWAITING_NICK, nick "" */
}

static void reply_error(struct msg *r, int code, const char *reason)
{
    memset(r, 0, sizeof *r);
    r->type = MT_ERROR;
    r->code = code;
    /* reason é sempre um dos literais do protocol.h/session.h, todos bem
     * abaixo de PROTO_REASON_MAX; o strncpy + '\0' é só cinto de segurança. */
    strncpy(r->reason, reason, sizeof r->reason - 1);
}

void session_handle(struct session *s, const char *line, size_t len,
                    struct msg *reply)
{
    struct msg       in;
    struct proto_err err;

    /* 1. A linha é uma mensagem válida? Se não, o protocolo já diz porquê. */
    if (proto_parse(line, len, &in, &err) != 0) {
        reply_error(reply, err.code, err.reason);
        return;
    }

    switch (in.type) {
    case MT_NICK:
        if (s->state == SESS_READY) {
            reply_error(reply, 400, SESS_R_NICK_SET);
            return;
        }
        /* in.nick já foi validado pelo protocolo e tem o mesmo tamanho. */
        memcpy(s->nick, in.nick, sizeof s->nick);
        s->state = SESS_READY;
        memset(reply, 0, sizeof *reply);
        reply->type = MT_OK;                                    /* D6 */
        return;

    case MT_MSG:
        if (s->state != SESS_READY) {
            reply_error(reply, 401, SESS_R_NICK_REQUIRED);      /* #17 */
            return;
        }
        memset(reply, 0, sizeof *reply);
        reply->type       = MT_CHAT;                            /* D5 */
        reply->is_private = 0;
        memcpy(reply->from, s->nick, sizeof reply->from);
        memcpy(reply->text, in.text, sizeof reply->text);
        return;

    default:
        /* OK, CHAT, ERROR, EVENT: válidos no protocolo, mas só no sentido
         * S->C. Um cliente que os envie está enganado, em qualquer estado. */
        reply_error(reply, 400, SESS_R_UNEXPECTED);
        return;
    }
}
