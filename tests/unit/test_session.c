/*
 * test_session.c — testes da máquina de estados do servidor.
 *
 * Linhas entram, respostas saem — sem rede. Cada resposta é também passada
 * pelo proto_encode() e comparada BYTE A BYTE com o que tem de ir para o fio:
 * é o mais perto que um teste unitário chega de uma sessão real.
 *
 * Numeração: o 17 vem da tabela em docs/milestones/M1.md; 33-39 acrescentados
 * com a máquina de estados.
 */
#include "session.h"
#include "test_util.h"

static struct session s;
static struct msg     r;
static char           wire[PROTO_LINE_BUF];

/* Trata uma linha e devolve a resposta tal como iria para o fio (com '\n').
 * Se o encode falhar, devolve "<ENCODE FALHOU>" — o que faz o CHECK_STR do
 * chamador falhar com uma mensagem legível. */
static const char *send_line(const char *line)
{
    session_handle(&s, line, strlen(line), &r);
    if (proto_encode(&r, wire, sizeof wire, NULL) != 0)
        return "<ENCODE FALHOU>";
    return wire;
}

#define OK_LINE "{\"type\":\"OK\"}\n"

/* ------------------------------------------------------------------------ */

/* 17. MSG antes de NICK -> 401, e o estado não muda. */
static void caso_17_msg_antes_de_nick(void)
{
    CASE(17, "MSG antes de NICK -> ERROR 401");
    session_init(&s);
    CHECK_STR(send_line("{\"type\":\"MSG\",\"text\":\"ola\"}"),
              "{\"type\":\"ERROR\",\"code\":401,\"reason\":\"nick required\"}\n");
    CHECK_INT(s.state, SESS_AWAITING_NICK);

    CASE(17, "e a seguir o NICK continua a funcionar");
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}"), OK_LINE);
}

/* 33. NICK valido -> OK (D6), passa a READY, nick guardado. */
static void caso_33_nick_ok(void)
{
    CASE(33, "NICK -> OK e READY");
    session_init(&s);
    CHECK_INT(s.state, SESS_AWAITING_NICK);
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}"), OK_LINE);
    CHECK_INT(s.state, SESS_READY);
    CHECK_STR(s.nick, "ana");
}

/* 34. MSG em READY -> CHAT ao proprio emissor (D5), com o nick DA SESSAO. */
static void caso_34_msg_vira_chat(void)
{
    CASE(34, "MSG -> CHAT com from = nick da sessao");
    session_init(&s);
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    CHECK_STR(send_line("{\"type\":\"MSG\",\"text\":\"hello\"}"),
              "{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"hello\",\"private\":false}\n");

    CASE(34, "um campo \"from\" mandado pelo cliente e ignorado (nao se faz passar por outro)");
    CHECK_STR(send_line("{\"type\":\"MSG\",\"from\":\"rui\",\"text\":\"oi\"}"),
              "{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"oi\",\"private\":false}\n");
}

/* 35. Linha invalida -> 400 com a razao do protocolo; estado intacto. */
static void caso_35_invalida_nao_muda_estado(void)
{
    CASE(35, "JSON invalido antes do NICK -> 400, continua AWAITING");
    session_init(&s);
    CHECK_STR(send_line("nao e json"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"invalid JSON\"}\n");
    CHECK_INT(s.state, SESS_AWAITING_NICK);

    CASE(35, "JSON invalido em READY -> 400, continua READY com o mesmo nick");
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    CHECK_STR(send_line("{\"type\":\"MSG\"}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"invalid field\"}\n");
    CHECK_INT(s.state, SESS_READY);
    CHECK_STR(s.nick, "ana");

    CASE(35, "tipo desconhecido -> 400 unknown type");
    CHECK_STR(send_line("{\"type\":\"LIST\"}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"unknown type\"}\n");
}

/* 36. Segundo NICK -> 400; o nick NAO muda. */
static void caso_36_segundo_nick(void)
{
    CASE(36, "NICK depois de registado -> 400 nick already set");
    session_init(&s);
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"rui\"}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"nick already set\"}\n");
    CHECK_STR(s.nick, "ana");
}

/* 37. Tipos S->C vindos de um cliente -> 400, nos DOIS estados (nao 401). */
static void caso_37_tipos_do_servidor(void)
{
    const char *esperado = "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"unexpected type\"}\n";

    CASE(37, "OK / CHAT / ERROR antes do NICK -> 400 (nao 401)");
    session_init(&s);
    CHECK_STR(send_line("{\"type\":\"OK\"}"), esperado);
    CHECK_STR(send_line("{\"type\":\"CHAT\",\"from\":\"x\",\"text\":\"y\"}"), esperado);
    CHECK_STR(send_line("{\"type\":\"ERROR\",\"code\":500}"), esperado);
    CHECK_INT(s.state, SESS_AWAITING_NICK);

    CASE(37, "e depois do NICK tambem");
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    CHECK_STR(send_line("{\"type\":\"OK\"}"), esperado);
    CHECK_INT(s.state, SESS_READY);
}

/* 38. NICK invalido (33 bytes) -> 400, continua AWAITING; um valido entra. */
static void caso_38_nick_invalido(void)
{
    CASE(38, "NICK com 33 bytes -> 400, sem registar");
    session_init(&s);
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"123456789012345678901234567890123\"}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"invalid field\"}\n");
    CHECK_INT(s.state, SESS_AWAITING_NICK);
    CHECK_STR(s.nick, "");

    CASE(38, "a seguir, um NICK valido regista");
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}"), OK_LINE);
}

/* 39. Uma sessao completa, como a do checkpoint de interop do M1.md. */
static void caso_39_sessao_completa(void)
{
    CASE(39, "sessao completa: erro, NICK, MSG, MSG com aspas");
    session_init(&s);
    CHECK_STR(send_line("{\"type\":\"MSG\",\"text\":\"cedo demais\"}"),
              "{\"type\":\"ERROR\",\"code\":401,\"reason\":\"nick required\"}\n");
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}"), OK_LINE);
    CHECK_STR(send_line("{\"type\":\"MSG\",\"text\":\"ola\"}"),
              "{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"ola\",\"private\":false}\n");
    CHECK_STR(send_line("{\"type\":\"MSG\",\"text\":\"diz \\\"oi\\\"\"}"),
              "{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"diz \\\"oi\\\"\",\"private\":false}\n");
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    caso_17_msg_antes_de_nick();
    caso_33_nick_ok();
    caso_34_msg_vira_chat();
    caso_35_invalida_nao_muda_estado();
    caso_36_segundo_nick();
    caso_37_tipos_do_servidor();
    caso_38_nick_invalido();
    caso_39_sessao_completa();
    return t_report("session");
}
