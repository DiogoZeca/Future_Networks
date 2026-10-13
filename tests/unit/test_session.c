/*
 * test_session.c — testes da máquina de estados do servidor.
 *
 * Linhas entram, saídas saem — sem rede, sem roster. Cada saída é também
 * passada pelo proto_encode() e comparada BYTE A BYTE com o que tem de ir para
 * o fio: é o mais perto que um teste unitário chega de uma sessão real.
 *
 * Numeração: o 17 vem da tabela em docs/milestones/M1.md; 33-39 acrescentados
 * com a máquina de estados (M1); 48-52 com os destinos e os EVENT (M2).
 */
#include "session.h"
#include "test_util.h"

static struct session  s;
static struct sess_out outs[SESS_MAX_OUT];
static size_t          n_outs;
static char            wire[PROTO_LINE_BUF];

/* Trata uma linha e devolve a PRIMEIRA saída tal como iria para o fio (com
 * '\n'). O número de saídas fica em n_outs e os destinos em outs[i].dest, para
 * os casos que os verificam.
 *
 * Devolve "<SEM SAIDAS>" ou "<ENCODE FALHOU>" em vez de rebentar: um teste tem
 * de falhar com um relatório, nunca com um segfault. */
static const char *send_line(const char *line)
{
    n_outs = session_handle(&s, line, strlen(line), outs, SESS_MAX_OUT);
    if (n_outs == 0)
        return "<SEM SAIDAS>";
    if (proto_encode(&outs[0].msg, wire, sizeof wire, NULL) != 0)
        return "<ENCODE FALHOU>";
    return wire;
}

/* A saída `i` codificada, para os casos que olham para a segunda. */
static const char *wire_of(size_t i)
{
    if (i >= n_outs)
        return "<NAO EXISTE>";
    if (proto_encode(&outs[i].msg, wire, sizeof wire, NULL) != 0)
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

/* 34. MSG em READY -> CHAT com o nick DA SESSAO. */
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

    CASE(36, "e nao gera EVENT nenhum");
    CHECK_INT(n_outs, 1);
}

/* 37. Tipos S->C vindos de um cliente -> 400, nos DOIS estados (nao 401). */
static void caso_37_tipos_do_servidor(void)
{
    const char *esperado = "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"unexpected type\"}\n";

    CASE(37, "OK / CHAT / ERROR / EVENT antes do NICK -> 400 (nao 401)");
    session_init(&s);
    CHECK_STR(send_line("{\"type\":\"OK\"}"), esperado);
    CHECK_STR(send_line("{\"type\":\"CHAT\",\"from\":\"x\",\"text\":\"y\"}"), esperado);
    CHECK_STR(send_line("{\"type\":\"ERROR\",\"code\":500}"), esperado);
    CHECK_STR(send_line("{\"type\":\"EVENT\",\"text\":\"x joined\"}"), esperado);
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

/* ---- M2: destinos e EVENT ----------------------------------------------- */

/* 48. Um NICK aceite produz DUAS saidas, com destinos diferentes.
 *
 *     A ordem importa: o OK vai primeiro, para que quem entra saiba que foi
 *     aceite antes de comecar a receber trafego dos outros. */
static void caso_48_nick_gera_ok_e_event(void)
{
    CASE(48, "NICK -> 2 saidas");
    session_init(&s);
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}"), OK_LINE);
    CHECK_INT(n_outs, 2);

    CASE(48, "a 1.a e o OK, so para quem entrou");
    CHECK_INT(outs[0].dest, SESS_SELF);
    CHECK_INT(outs[0].msg.type, MT_OK);

    CASE(48, "a 2.a e o EVENT, para os OUTROS (quem entra nao precisa de aviso)");
    CHECK_INT(outs[1].dest, SESS_OTHERS);
    CHECK_INT(outs[1].msg.type, MT_EVENT);
    CHECK_STR(wire_of(1), "{\"type\":\"EVENT\",\"text\":\"ana joined\"}\n");
}

/* 49. Um MSG vai para TODOS, incluindo o emissor (D5).
 *
 *     SESS_ALL e nao SESS_OTHERS: e o proprio CHAT de volta que confirma ao
 *     emissor que a mensagem chegou ao servidor — e e por isso que o cliente
 *     nao deve ecoar localmente o que escreve. */
static void caso_49_msg_vai_para_todos(void)
{
    CASE(49, "MSG -> 1 saida, destino ALL (D5)");
    session_init(&s);
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    send_line("{\"type\":\"MSG\",\"text\":\"ola\"}");
    CHECK_INT(n_outs, 1);
    CHECK_INT(outs[0].dest, SESS_ALL);
    CHECK_INT(outs[0].msg.type, MT_CHAT);
}

/* 50. Os erros sao SEMPRE so para quem se enganou: um cliente com um bug nao
 *     deve encher o chat de toda a gente com ERROR. */
static void caso_50_erros_so_para_o_proprio(void)
{
    CASE(50, "erro antes do NICK -> SELF");
    session_init(&s);
    send_line("{\"type\":\"MSG\",\"text\":\"cedo\"}");
    CHECK_INT(n_outs, 1);
    CHECK_INT(outs[0].dest, SESS_SELF);

    CASE(50, "JSON invalido -> SELF");
    send_line("nao e json");
    CHECK_INT(outs[0].dest, SESS_SELF);

    CASE(50, "segundo NICK -> SELF");
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    send_line("{\"type\":\"NICK\",\"nick\":\"rui\"}");
    CHECK_INT(outs[0].dest, SESS_SELF);
}

/* 51. O EVENT de saida. */
static void caso_51_leave(void)
{
    struct msg m;

    CASE(51, "quem saiu depois do NICK tem EVENT de saida");
    session_init(&s);
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    CHECK_INT(session_leave(&s, &m), 1);
    CHECK_INT(m.type, MT_EVENT);
    CHECK_INT(proto_encode(&m, wire, sizeof wire, NULL), 0);
    CHECK_STR(wire, "{\"type\":\"EVENT\",\"text\":\"ana left\"}\n");

    CASE(51, "quem sai ANTES do NICK nao gera EVENT: ninguem sabia que estava la");
    session_init(&s);
    CHECK_INT(session_leave(&s, &m), 0);

    CASE(51, "um NICK recusado tambem nao conta como entrada");
    session_init(&s);
    send_line("{\"type\":\"NICK\",\"nick\":\"a b\"}");      /* espaco: invalido */
    CHECK_INT(session_leave(&s, &m), 0);
}

/* 52. Um nick no limite (32 bytes) ainda produz EVENT validos e codificaveis.
 *     O _Static_assert do session.c garante que cabe; isto executa-o. */
static void caso_52_nick_no_limite(void)
{
    const char *n32 = "12345678901234567890123456789012";   /* 32 */

    CASE(52, "nick de 32 bytes: EVENT de entrada cabe e codifica");
    session_init(&s);
    char line[128];
    snprintf(line, sizeof line, "{\"type\":\"NICK\",\"nick\":\"%s\"}", n32);
    send_line(line);
    CHECK_INT(n_outs, 2);
    CHECK_INT(proto_encode(&outs[1].msg, wire, sizeof wire, NULL), 0);

    CASE(52, "e o de saida tambem");
    struct msg m;
    CHECK_INT(session_leave(&s, &m), 1);
    CHECK_INT(proto_encode(&m, wire, sizeof wire, NULL), 0);
    CHECK_STR(wire, "{\"type\":\"EVENT\",\"text\":"
                    "\"12345678901234567890123456789012 left\"}\n");
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
    caso_48_nick_gera_ok_e_event();
    caso_49_msg_vai_para_todos();
    caso_50_erros_so_para_o_proprio();
    caso_51_leave();
    caso_52_nick_no_limite();
    return t_report("session");
}
