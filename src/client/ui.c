/*
 * ui.c — conversões do cliente, sem sockets. Contrato em include/ui.h.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

int ui_input_to_msg(const char *typed, struct msg *out, const char **why)
{
    memset(out, 0, sizeof *out);

    /* Tirar o fim de linha: "\n" ou "\r\n" (terminais e pipes do Windows). */
    size_t n = strcspn(typed, "\n");
    if (n > 0 && typed[n - 1] == '\r')
        n--;

    if (n == 0)
        return 0;                           /* Enter sozinho: nada a enviar */

    if (n > PROTO_TEXT_MAX) {
        *why = UI_WHY_TOOLONG;
        return -1;
    }

    memcpy(out->text, typed, n);            /* n <= 1024 < sizeof text */
    out->text[n] = '\0';

    if (!proto_valid_text(out->text)) {     /* só pode ser controlo: o tamanho já passou */
        memset(out, 0, sizeof *out);
        *why = UI_WHY_CONTROL;
        return -1;
    }

    out->type = MT_MSG;
    return 1;
}

void ui_format(const struct msg *m, char *out, size_t cap)
{
    if (cap == 0)
        return;

    /* snprintf trunca e termina sempre em '\0': nunca escreve fora de out. */
    switch (m->type) {
    case MT_CHAT:
        snprintf(out, cap, "<%s> %s%s", m->from,
                 m->is_private ? "(privado) " : "", m->text);
        break;
    case MT_ERROR:
        if (m->reason[0] != '\0')
            snprintf(out, cap, "[erro %d] %s", m->code, m->reason);
        else
            snprintf(out, cap, "[erro %d]", m->code);
        break;
    case MT_OK:
        snprintf(out, cap, "[ok]");
        break;
    case MT_EVENT:
        /* Entradas e saídas (M2). O '*' à frente distingue à vista um aviso do
         * servidor de uma mensagem de alguém, que leva "<nick>". */
        snprintf(out, cap, "* %s", m->text);
        break;
    default:
        snprintf(out, cap, "[?]");
        break;
    }
}
