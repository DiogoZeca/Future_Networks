/*
 * ui.c — conversões do cliente, sem sockets. Contrato em include/ui.h.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

/* Copia text[0..n) para dst como texto de mensagem e valida-o. Devolve
 * UI_SEND ou UI_BAD (com *why). dst tem PROTO_TEXT_MAX + 1 bytes. */
static int take_text(const char *text, size_t n, char *dst, const char **why)
{
    if (n > PROTO_TEXT_MAX) {
        *why = UI_WHY_TOOLONG;
        return UI_BAD;
    }
    memcpy(dst, text, n);                   /* n <= 4096 < PROTO_TEXT_MAX + 1 */
    dst[n] = '\0';
    /* Desde a revisão do M3 o text pode ter caracteres de controlo (um tab,
     * por exemplo): só o tamanho conta, e esse já passou. */
    return UI_SEND;
}

/* "/cmd" sozinho (ou com espaços a seguir)? */
static int is_cmd(const char *line, size_t n, const char *cmd, int *has_args)
{
    size_t k = strlen(cmd);
    if (n < k || memcmp(line, cmd, k) != 0 || (n > k && line[k] != ' '))
        return 0;
    *has_args = 0;
    for (size_t i = k; i < n; i++)
        if (line[i] != ' ')
            *has_args = 1;
    return 1;
}

/* "/msg <nick> <texto>": parte no PRIMEIRO espaço depois do nick (os nicks
 * não têm espaços — foi por isso que o D4 os proibiu no M1). O texto fica tal
 * e qual, com os espaços que tiver. */
static int parse_msg_cmd(const char *line, size_t n, struct msg *out, const char **why)
{
    const char *p   = line + 5;             /* depois de "/msg " */
    const char *end = line + n;

    while (p < end && *p == ' ')            /* "/msg   rui oi" também serve */
        p++;
    const char *nick = p;
    while (p < end && *p != ' ')
        p++;
    size_t nick_len = (size_t)(p - nick);
    if (nick_len == 0 || p >= end) {        /* sem nick, ou sem texto */
        *why = UI_WHY_MSG_USE;
        return UI_BAD;
    }
    if (nick_len > PROTO_NICK_MAX) {
        *why = UI_WHY_BADNICK;
        return UI_BAD;
    }
    memcpy(out->to, nick, nick_len);
    out->to[nick_len] = '\0';
    if (!proto_valid_nick(out->to)) {
        *why = UI_WHY_BADNICK;
        return UI_BAD;
    }

    p++;                                    /* o espaço separador */
    if (p >= end) {                         /* "/msg rui " sem texto */
        *why = UI_WHY_MSG_USE;
        return UI_BAD;
    }
    if (take_text(p, (size_t)(end - p), out->text, why) != UI_SEND)
        return UI_BAD;

    out->type = MT_PRIV;
    return UI_SEND;
}

int ui_input_to_msg(const char *typed, struct msg *out, const char **why)
{
    memset(out, 0, sizeof *out);

    /* Tirar o fim de linha: "\n" ou "\r\n" (terminais e pipes do Windows). */
    size_t n = strcspn(typed, "\n");
    if (n > 0 && typed[n - 1] == '\r')
        n--;

    if (n == 0)
        return UI_NONE;                     /* Enter sozinho: nada a enviar */

    int rc     = UI_BAD;
    int args   = 0;

    if (typed[0] != '/') {                  /* o caso normal: para todos */
        rc = take_text(typed, n, out->text, why);
        if (rc == UI_SEND)
            out->type = MT_MSG;
    } else if (n >= 2 && typed[1] == '/') { /* "//x" -> MSG "/x" */
        rc = take_text(typed + 1, n - 1, out->text, why);
        if (rc == UI_SEND)
            out->type = MT_MSG;
    } else if (is_cmd(typed, n, "/list", &args) || is_cmd(typed, n, "/quit", &args)) {
        if (args) {
            *why = UI_WHY_ARGS;
        } else {
            out->type = (typed[1] == 'l') ? MT_LIST : MT_QUIT;
            rc = UI_SEND;
        }
    } else if (is_cmd(typed, n, "/help", &args)) {
        rc = UI_HELP;
    } else if (is_cmd(typed, n, "/msg", &args)) {
        rc = args ? parse_msg_cmd(typed, n, out, why) : UI_BAD;
        if (!args)
            *why = UI_WHY_MSG_USE;
    } else {
        *why = UI_WHY_UNKNOWN;
    }

    if (rc != UI_SEND)
        memset(out, 0, sizeof *out);        /* nunca meio preenchido */
    return rc;
}

/* Acrescenta a out[pos..] o texto `t`, LIMPO para o terminal, e devolve a
 * nova posição. out fica sempre terminado em '\0' e nunca se escreve para lá
 * de out[cap-1].
 *
 *   '\n'          -> "\n" + INDENT  (as linhas seguintes ficam alinhadas)
 *   '\t'          -> fica
 *   outro controlo -> '?'           ('\033' mudaria o terminal; '\r' voltava
 *                                    ao início da linha e escrevia por cima)
 * Bytes >= 0x80 (UTF-8) passam: não são controlo. */
#define UI_INDENT "    "
static size_t put_text(char *out, size_t cap, size_t pos, const char *t)
{
    for (; *t != '\0' && pos + 1 < cap; t++) {
        unsigned char c = (unsigned char)*t;
        if (c == '\n') {
            out[pos++] = '\n';
            for (const char *i = UI_INDENT; *i != '\0' && pos + 1 < cap; i++)
                out[pos++] = *i;
        } else if (c == '\t' || (c >= 0x20 && c != 0x7F)) {
            out[pos++] = (char)c;
        } else {
            out[pos++] = '?';
        }
    }
    out[pos] = '\0';
    return pos;
}

/* snprintf que devolve onde ficou (sem nunca passar de cap - 1). */
static size_t put_fmt_len(size_t cap, int n)
{
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

void ui_format(const struct msg *m, char *out, size_t cap)
{
    if (cap == 0)
        return;
    out[0] = '\0';

    size_t pos = 0;
    /* snprintf trunca e termina sempre em '\0': nunca escreve fora de out. */
    switch (m->type) {
    case MT_CHAT:
        if (m->is_private && m->to[0] != '\0')      /* eco local do /msg */
            pos = put_fmt_len(cap, snprintf(out, cap, "<%s -> %s> (privado) ", m->from, m->to));
        else
            pos = put_fmt_len(cap, snprintf(out, cap, "<%s> %s", m->from,
                                            m->is_private ? "(privado) " : ""));
        pos = put_text(out, cap, pos, m->text);
        break;
    case MT_USERS:
        /* Os nicks já vêm separados por um espaço (protocol.h); de um
         * servidor podem ter '"' ou '\\', mas nunca controlo. */
        if (m->n_nicks == 0)
            pos = put_fmt_len(cap, snprintf(out, cap, "[online: 0]"));
        else
            pos = put_fmt_len(cap, snprintf(out, cap, "[online: %zu] %s", m->n_nicks, m->nicks));
        break;
    case MT_ERROR:
        pos = put_fmt_len(cap, snprintf(out, cap, "[erro %d]%s", m->code,
                                        m->reason[0] != '\0' ? " " : ""));
        pos = put_text(out, cap, pos, m->reason);
        break;
    case MT_OK:
        pos = put_fmt_len(cap, snprintf(out, cap, "[ok]"));
        break;
    case MT_EVENT:
        /* Entradas e saídas (M2). O '*' à frente distingue à vista um aviso do
         * servidor de uma mensagem de alguém, que leva "<nick>". */
        pos = put_fmt_len(cap, snprintf(out, cap, "* "));
        pos = put_text(out, cap, pos, m->text);
        break;
    default:
        pos = put_fmt_len(cap, snprintf(out, cap, "[?]"));
        break;
    }

    /* O protocolo teve de cortar alguma coisa (tolerante a receber). */
    if (m->truncated && pos + 1 < cap)
        put_fmt_len(cap - pos, snprintf(out + pos, cap - pos, " [...]"));
}
