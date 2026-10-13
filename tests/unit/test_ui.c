/*
 * test_ui.c — testes da parte pura do cliente (include/ui.h).
 *
 * Numeração: 40-46, a seguir aos da session.
 */
#include "ui.h"
#include "test_util.h"

static struct msg  m;
static const char *why;
static char        typed[PROTO_TEXT_MAX + 16];
static char        shown[PROTO_LINE_BUF];

/* 40. Linhas normais: o fim de linha sai, o texto fica intacto. */
static void caso_40_linha_normal(void)
{
    CASE(40, "\"ola\\n\" -> MSG \"ola\"");
    CHECK_INT(ui_input_to_msg("ola\n", &m, &why), 1);
    CHECK_INT(m.type, MT_MSG);
    CHECK_STR(m.text, "ola");

    CASE(40, "\"\\r\\n\" tambem sai");
    CHECK_INT(ui_input_to_msg("ola\r\n", &m, &why), 1);
    CHECK_STR(m.text, "ola");

    CASE(40, "sem '\\n' (ultima linha antes do EOF)");
    CHECK_INT(ui_input_to_msg("fim", &m, &why), 1);
    CHECK_STR(m.text, "fim");

    CASE(40, "espacos e UTF-8 sao texto normal");
    CHECK_INT(ui_input_to_msg("  ol\xC3\xA1 mundo  \n", &m, &why), 1);
    CHECK_STR(m.text, "  ol\xC3\xA1 mundo  ");
}

/* 41. Enter sozinho: nao envia nada (0), nao e erro. */
static void caso_41_linha_vazia(void)
{
    CASE(41, "linha vazia -> 0");
    CHECK_INT(ui_input_to_msg("\n", &m, &why), 0);
    CHECK_INT(ui_input_to_msg("\r\n", &m, &why), 0);
    CHECK_INT(ui_input_to_msg("", &m, &why), 0);
    CHECK_INT(m.type, 0);
}

/* 42. Limite do texto, dos dois lados, com motivo concreto. */
static void caso_42_limite(void)
{
    CASE(42, "1024 bytes + '\\n' aceite");
    memset(typed, 'x', PROTO_TEXT_MAX);
    strcpy(typed + PROTO_TEXT_MAX, "\n");
    CHECK_INT(ui_input_to_msg(typed, &m, &why), 1);
    CHECK_INT(strlen(m.text), PROTO_TEXT_MAX);

    CASE(42, "1025 bytes -> -1 'demasiado longa'");
    memset(typed, 'x', PROTO_TEXT_MAX + 1);
    strcpy(typed + PROTO_TEXT_MAX + 1, "\n");
    why = NULL;
    CHECK_INT(ui_input_to_msg(typed, &m, &why), -1);
    CHECK_STR(why, UI_WHY_TOOLONG);
    CHECK_INT(m.type, 0);
}

/* 43. Caracteres de controlo: recusados com motivo, antes de chegar ao fio. */
static void caso_43_controlo(void)
{
    CASE(43, "tab -> -1 'caracteres de controlo'");
    why = NULL;
    CHECK_INT(ui_input_to_msg("a\tb\n", &m, &why), -1);
    CHECK_STR(why, UI_WHY_CONTROL);
    CHECK_INT(m.type, 0);

    CASE(43, "o que passa no ui passa no encode (mesmas regras)");
    CHECK_INT(ui_input_to_msg("diz \"oi\" \\\n", &m, &why), 1);
    CHECK_INT(proto_encode(&m, shown, sizeof shown, NULL), 0);
}

/* 44. Formato de cada tipo recebido. */
static void caso_44_formato(void)
{
    CASE(44, "CHAT");
    struct msg c = { .type = MT_CHAT, .from = "ana", .text = "hello" };
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> hello");

    CASE(44, "CHAT privado");
    c.is_private = 1;
    ui_format(&c, shown, sizeof shown);
    CHECK_STR(shown, "<ana> (privado) hello");

    CASE(44, "ERROR com e sem reason");
    struct msg e = { .type = MT_ERROR, .code = 401, .reason = "nick required" };
    ui_format(&e, shown, sizeof shown);
    CHECK_STR(shown, "[erro 401] nick required");
    e.reason[0] = '\0';
    ui_format(&e, shown, sizeof shown);
    CHECK_STR(shown, "[erro 401]");

    CASE(44, "OK e tipo nenhum");
    struct msg o = { .type = MT_OK };
    ui_format(&o, shown, sizeof shown);
    CHECK_STR(shown, "[ok]");
    struct msg z = { .type = 0 };
    ui_format(&z, shown, sizeof shown);
    CHECK_STR(shown, "[?]");
}

/* 45. Buffer pequeno: trunca, termina em '\0', nao escreve fora. Sob SAN=1
 *     o ASan confirma que nao ha escrita depois de out[cap-1]. */
static void caso_45_truncar(void)
{
    CASE(45, "cap pequeno trunca com '\\0'");
    char small[6];
    struct msg c = { .type = MT_CHAT, .from = "ana", .text = "hello" };
    ui_format(&c, small, sizeof small);
    CHECK_STR(small, "<ana>");

    CASE(45, "cap 0 nao escreve nada");
    char guard = 'G';
    ui_format(&c, &guard, 0);
    CHECK_INT(guard, 'G');
}

/* 46. Validacao do nick da linha de comando (antes de ligar). */
static void caso_46_nick_argv(void)
{
    CASE(46, "proto_valid_nick para o nick do argv");
    CHECK_INT(proto_valid_nick("ana"), 1);
    CHECK_INT(proto_valid_nick("a b"), 0);
    CHECK_INT(proto_valid_nick(""), 0);
    CHECK_INT(proto_valid_nick("123456789012345678901234567890123"), 0);
}

int main(void)
{
    caso_40_linha_normal();
    caso_41_linha_vazia();
    caso_42_limite();
    caso_43_controlo();
    caso_44_formato();
    caso_45_truncar();
    caso_46_nick_argv();
    return t_report("ui");
}
