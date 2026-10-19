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

/* ---- Editor de linha (correção do M3) ----------------------------------- */

/* Byte de continuação UTF-8 (10xxxxxx): o meio de um carácter. */
static int is_cont(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/* Quantos bytes tem o carácter que começa com `c` (1 para ASCII e para um
 * byte inválido: o servidor recusa UTF-8 inválido, mas o editor não tem de
 * adivinhar o que a pessoa queria). */
static int utf8_need(unsigned char c)
{
    if (c >= 0xC0 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF7) return 4;
    return 1;
}

/* Apaga o último carácter inteiro (os bytes de continuação e o que os começa). */
static void erase_char(struct ui_line *l)
{
    l->want = 0;                                /* um carácter a meio sai inteiro */
    l->part = 0;
    while (l->len > 0 && is_cont((unsigned char)l->buf[l->len - 1]))
        l->len--;
    if (l->len > 0)
        l->len--;
    l->buf[l->len] = '\0';
}

void ui_line_init(struct ui_line *l)
{
    memset(l, 0, sizeof *l);
}

/* Deita fora um carácter UTF-8 que ficou a meio (chegou o 1.º byte e não
 * chegaram os outros). */
static void drop_partial(struct ui_line *l)
{
    if (l->part > 0) {
        l->len -= (size_t)l->part;
        l->buf[l->len] = '\0';
    }
    l->part = 0;
    l->want = 0;
}

int ui_line_key(struct ui_line *l, unsigned char c)
{
    /* 1. A meio de uma sequência de escape (setas, Delete, F1...): engolir
     *    até ao fim dela. "ESC [" e "ESC O" acabam num byte 0x40-0x7E; o
     *    que vem entre eles (0x20-0x3F) são parâmetros. Sem isto, uma seta
     *    para cima escrevia "[A" na linha. */
    if (l->esc == 1) {
        l->esc = (c == '[' || c == 'O') ? 2 : 0;   /* Alt+tecla: engolir a tecla */
        return UI_KEY_NONE;
    }
    if (l->esc == 2) {
        if (c >= 0x40 && c <= 0x7E)
            l->esc = 0;
        return UI_KEY_NONE;
    }

    /* 2. Um carácter UTF-8 chega byte a byte. Os de continuação completam o
     *    que está a meio (e só quando fica completo é que vale a pena
     *    redesenhar: meio carácter no ecrã é lixo). Um de continuação sem
     *    nada a meio é deitado fora — a linha fica sempre UTF-8 válido. */
    if (is_cont(c)) {
        if (l->drop > 0) {                      /* resto de um que não coube */
            l->drop--;
            return UI_KEY_NONE;
        }
        if (l->want == 0)                       /* solto: ignorar */
            return UI_KEY_NONE;
        l->buf[l->len++] = (char)c;             /* coube: reservado no 1.º byte */
        l->buf[l->len]   = '\0';
        l->part++;
        if (--l->want > 0)
            return UI_KEY_NONE;
        l->part = 0;                            /* completo */
        return UI_KEY_REDRAW;
    }
    l->drop = 0;
    int dropped = 0;
    if (l->want > 0) {                          /* o anterior ficou a meio */
        drop_partial(l);
        dropped = 1;
    }

    switch (c) {
    case 0x1B:                                  /* ESC */
        l->esc = 1;
        return UI_KEY_NONE;
    case '\r':
    case '\n':                                  /* Enter */
        /* Linha vazia: não enviar E não mudar de linha (era o "spam de
         * Enters": cada um fazia subir o ecrã). */
        return l->len == 0 ? UI_KEY_NONE : UI_KEY_SUBMIT;
    case 0x7F:
    case 0x08:                                  /* Backspace */
        if (dropped)                            /* já apagou o que estava a meio */
            return UI_KEY_NONE;
        if (l->len == 0)
            return UI_KEY_NONE;
        erase_char(l);
        return UI_KEY_REDRAW;
    case 0x15:                                  /* Ctrl+U: apagar a linha */
        if (l->len == 0)
            return UI_KEY_NONE;
        l->len    = 0;
        l->buf[0] = '\0';
        return UI_KEY_REDRAW;
    case 0x17:                                  /* Ctrl+W: apagar a palavra */
        if (l->len == 0)
            return UI_KEY_NONE;
        while (l->len > 0 && l->buf[l->len - 1] == ' ')
            l->len--;
        while (l->len > 0 && l->buf[l->len - 1] != ' ')
            l->len--;
        l->buf[l->len] = '\0';
        return UI_KEY_REDRAW;
    case 0x0C:                                  /* Ctrl+L */
        return UI_KEY_REDRAW;
    case 0x04:                                  /* Ctrl+D */
        return l->len == 0 ? UI_KEY_EOF : UI_KEY_NONE;
    default:
        break;
    }

    if (c < 0x20 || c >= 0xF8)                  /* Tab, controlos, bytes que
                                                   nunca são UTF-8 */
        return UI_KEY_NONE;

    /* 3. O 1.º byte de um carácter. Ou cabe INTEIRO, ou não entra nenhum
     *    byte dele: com o limite a meio de um "é", ficava só o 1.º byte e a
     *    linha deixava de ser UTF-8 válido (o servidor responderia 400). */
    int need = utf8_need(c);
    if (l->len + (size_t)need > UI_LINE_MAX) {
        l->drop = need - 1;
        return UI_KEY_FULL;
    }
    l->buf[l->len++] = (char)c;
    l->buf[l->len]   = '\0';
    if (need > 1) {
        l->want = need - 1;
        l->part = 1;
        return UI_KEY_NONE;                     /* redesenhar quando completo */
    }
    return UI_KEY_REDRAW;
}

size_t ui_line_tail(const struct ui_line *l, size_t cols)
{
    size_t end = ui_line_shown(l);
    if (cols == 0)
        return end;
    size_t n = 0;
    for (size_t pos = end; pos > 0; ) {
        pos--;
        if (!is_cont((unsigned char)l->buf[pos]) && ++n == cols)
            return pos;                         /* este carácter ainda cabe */
    }
    return 0;                                   /* cabe tudo */
}

size_t ui_line_shown(const struct ui_line *l)
{
    return l->len - (size_t)l->part;
}

/* ---- M5 ----------------------------------------------------------------- */

int ui_parse_choice(const char *typed, size_t n)
{
    const char *p = typed;
    while (*p == ' ' || *p == '\t')
        p++;
    size_t v = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (size_t)(*p - '0');
        if (++digits > 6)                   /* ninguém tem 1000000 servidores */
            return 0;
        p++;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (digits == 0 || *p != '\0' || v < 1 || v > n)
        return 0;
    return (int)v;
}
