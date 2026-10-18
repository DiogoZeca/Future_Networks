/*
 * session.c — máquina de estados de uma ligação. Contrato em include/session.h.
 */
#include "session.h"

#include <stdio.h>
#include <stdlib.h>   /* qsort */
#include <string.h>

/* Um nick cabe sempre num text com folga: 32 + strlen(" joined") = 39, muito
 * abaixo de 1024. Se os limites mudarem, isto deixa de compilar em vez de
 * truncar o EVENT em silêncio. */
_Static_assert(PROTO_NICK_MAX + sizeof(" joined") <= PROTO_TEXT_MAX,
               "o texto de um EVENT tem de caber num text");
_Static_assert(PROTO_NICK_MAX + sizeof(SESS_EV_WELCOME) <= PROTO_TEXT_MAX,
               "as boas-vindas (D22) têm de caber num text");

void session_init(struct session *s, const struct sess_dir *dir)
{
    memset(s, 0, sizeof *s);                /* AWAITING_NICK, nick "" */
    s->dir = dir;
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

/* Uma saída "simples" (sem campos) para quem enviou a linha: o OK. */
static void fill_self(struct sess_out *o, enum msg_type t)
{
    memset(o, 0, sizeof *o);
    o->dest     = SESS_SELF;
    o->msg.type = t;
}

static int cmp_nicks(const void *a, const void *b)
{
    return proto_nick_cmp(a, b);            /* cada elemento é um char[33] */
}

/* LIST → USERS (D16): todos os que estão no chat, incluindo o próprio, por
 * ordem alfabética. A ordem é decidida AQUI e não no roster: o roster devolve
 * pela ordem da lista, que muda a cada saída (swap com o último). */
static size_t handle_list(struct session *s, struct sess_out *out)
{
    char   nicks[PROTO_USERS_MAX][PROTO_NICK_MAX + 1];
    size_t n = s->dir->nicks(s->dir->ctx, nicks, PROTO_USERS_MAX);

    qsort(nicks, n, sizeof nicks[0], cmp_nicks);

    fill_self(&out[0], MT_USERS);
    for (size_t i = 0; i < n; i++)
        proto_users_add(&out[0].msg, nicks[i]);   /* já validados no NICK */
    return 1;
}

/* PRIV → OK ao emissor + CHAT privado ao destino (D13, D14, D15). */
static size_t handle_priv(struct session *s, const struct msg *in,
                          struct sess_out *out, size_t cap)
{
    char to[PROTO_NICK_MAX + 1];

    /* "Existe?" no instante do pedido. Se o destino sair entre esta pergunta
     * e o envio, a mensagem perde-se sem erro — como um CHAT para quem saiu
     * a meio de um broadcast. O pedido foi aceite (OK) na mesma. */
    if (!s->dir->lookup(s->dir->ctx, in->to, to))
        return only_error(out, 404, SESS_R_NO_SUCH_NICK);

    fill_self(&out[0], MT_OK);              /* D13: o OK vai primeiro */
    if (cap < 2)
        return 1;

    memset(&out[1], 0, sizeof out[1]);
    out[1].msg.type       = MT_CHAT;
    out[1].msg.is_private = 1;
    /* from da SESSÃO (como no MSG). O msg.to (como foi REGISTADO: "/msg ANA"
     * encontra "ana", D18) só diz ao main.c para onde encaminhar: não vai
     * para o fio (protocol.c). */
    memcpy(out[1].msg.from, s->nick, sizeof s->nick);   /* from é maior: ver protocol.h */
    memcpy(out[1].msg.to,   to,      sizeof out[1].msg.to);
    memcpy(out[1].msg.text, in->text, sizeof out[1].msg.text);

    /* D14: só o destino recebe o CHAT, nunca uma cópia ao emissor. A si
     * próprio, o destino É o emissor: SELF, para não ir pelo roster. */
    out[1].dest = proto_nick_eq(to, s->nick) ? SESS_SELF : SESS_TO;
    return 2;
}

size_t session_handle(struct session *s, const char *line, size_t len,
                      struct sess_out *out, size_t cap)
{
    struct msg       in;
    struct proto_err err;

    if (cap == 0 || s->state == SESS_CLOSED)
        return 0;

    /* 1. A linha é uma mensagem válida? Se não, o protocolo já diz porquê. */
    if (proto_parse(line, len, &in, &err) != 0)
        return only_error(out, err.code, err.reason);

    switch (in.type) {
    case MT_NICK:
        if (s->state == SESS_READY)
            return only_error(out, 400, SESS_R_NICK_SET);

        /* Único (D18/D19). Verificar e reservar é UMA operação do diretório
         * (atómica no roster). Se falhar, o estado não muda: o cliente pode
         * tentar outro nick na mesma ligação. */
        if (!s->dir->claim(s->dir->ctx, in.nick))
            return only_error(out, 409, SESS_R_NICK_TAKEN);

        /* in.nick já foi validado pelo protocolo e tem o mesmo tamanho. */
        memcpy(s->nick, in.nick, sizeof s->nick);
        s->state = SESS_READY;

        /* Três saídas, e a ordem importa: o OK vai primeiro para que quem
         * entrou saiba que foi aceite antes de começar a receber tráfego. */
        memset(&out[0], 0, sizeof out[0]);
        out[0].dest     = SESS_SELF;
        out[0].msg.type = MT_OK;                                /* D6 */
        if (cap < 2)
            return 1;

        out[1].dest = SESS_OTHERS;   /* quem entra não precisa de ser avisado */
        fill_event(&out[1].msg, s->nick, "joined");
        if (cap < 3)
            return 2;

        /* D22 (correção do M3): as boas-vindas, só para quem entrou, com os
         * comandos do slide 123. É o "/help do lado do servidor" — não há um
         * tipo HELP no slide 124, e um EVENT qualquer cliente já sabe
         * mostrar. Vai DEPOIS do "joined" para não mudar o índice das saídas
         * que já existiam. */
        out[2].dest = SESS_SELF;
        memset(&out[2].msg, 0, sizeof out[2].msg);
        out[2].msg.type = MT_EVENT;
        snprintf(out[2].msg.text, sizeof out[2].msg.text, SESS_EV_WELCOME, s->nick);
        return 3;

    case MT_MSG:
        if (s->state != SESS_READY)
            return only_error(out, 401, SESS_R_NICK_REQUIRED);  /* #17 */

        /* D13: OK primeiro (o pedido foi aceite), depois o CHAT. */
        fill_self(&out[0], MT_OK);
        if (cap < 2)
            return 1;

        memset(&out[1], 0, sizeof out[1]);
        /* SESS_ALL e não SESS_OTHERS: o emissor recebe o seu próprio CHAT
         * (D5) — é por isso que o cliente não deve ecoar localmente o que
         * escreve para todos. */
        out[1].dest             = SESS_ALL;
        out[1].msg.type         = MT_CHAT;
        out[1].msg.is_private   = 0;
        /* O `from` vem da SESSÃO, nunca do que o cliente mandou: senão
         * qualquer um se fazia passar por outro. */
        memcpy(out[1].msg.from, s->nick, sizeof s->nick);   /* from é maior */
        memcpy(out[1].msg.text, in.text, sizeof out[1].msg.text);
        return 2;

    case MT_PRIV:
        if (s->state != SESS_READY)
            return only_error(out, 401, SESS_R_NICK_REQUIRED);
        return handle_priv(s, &in, out, cap);

    case MT_LIST:
        if (s->state != SESS_READY)
            return only_error(out, 401, SESS_R_NICK_REQUIRED);
        return handle_list(s, out);

    case MT_QUIT:
        /* Em QUALQUER estado (D17): sair tem de ser sempre possível, mesmo
         * a quem ainda não escolheu nick. O OK diz-lhe que o pedido chegou;
         * o "<nick> left" aos outros vem do session_leave(), como numa queda. */
        fill_self(&out[0], MT_OK);
        s->state = SESS_CLOSED;
        return 1;

    default:
        /* OK, CHAT, ERROR, EVENT, USERS: válidos no protocolo, mas só no
         * sentido S->C. Um cliente que os envie está enganado, em qualquer
         * estado. */
        return only_error(out, 400, SESS_R_UNEXPECTED);
    }
}

int session_leave(const struct session *s, struct msg *out)
{
    /* Saiu antes de escolher nick: ninguém sabia que estava cá, não há nada a
     * anunciar. Acontece sempre que alguém liga e desliga sem falar — um
     * scanner de portas, por exemplo. */
    if (s->nick[0] == '\0')                 /* nunca entrou (ou QUIT antes do NICK) */
        return 0;

    fill_event(out, s->nick, "left");
    return 1;
}

size_t session_timeout(const struct session *s, struct sess_out *out)
{
    /* Quem nunca fez NICK não esteve "inativo": nunca chegou a entrar. O
     * reason diz qual dos dois prazos acabou; o código é o mesmo (408). */
    return only_error(out, 408, s->state == SESS_READY ? SESS_R_IDLE
                                                       : SESS_R_NICK_TIMEOUT);
}

size_t session_shutdown(struct sess_out *out)
{
    memset(out, 0, sizeof *out);
    out->dest     = SESS_SELF;
    out->msg.type = MT_EVENT;
    snprintf(out->msg.text, sizeof out->msg.text, "%s", SESS_EV_SHUTDOWN);
    return 1;
}
