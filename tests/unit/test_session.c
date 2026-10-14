/*
 * test_session.c — testes da máquina de estados do servidor.
 *
 * Linhas entram, saídas saem — sem rede, sem roster. Cada saída é também
 * passada pelo proto_encode() e comparada BYTE A BYTE com o que tem de ir para
 * o fio: é o mais perto que um teste unitário chega de uma sessão real.
 *
 * Numeração: o 17 vem da tabela em docs/milestones/M1.md; 33-39 acrescentados
 * com a máquina de estados (M1); 48-52 com os destinos e os EVENT (M2);
 * 63-70 com os comandos do M3 (409, PRIV, LIST, QUIT).
 *
 * Desde o M3 a session pergunta coisas sobre os OUTROS a um "diretório"
 * (session.h). Aqui o diretório é um array — os "outros" que estão no chat —
 * e cada caso põe lá quem precisar. Continua sem rede, sem roster e sem
 * threads: é isso que o diretório compra.
 */
#include "session.h"
#include "test_util.h"

#include <stdio.h>

/* ---- Diretório falso ---------------------------------------------------- */

#define FAKE_MAX 8
static char   fake[FAKE_MAX][PROTO_NICK_MAX + 1];   /* quem está no chat */
static size_t n_fake;

static void fake_reset(void) { n_fake = 0; }
static void fake_add(const char *nick)
{
    snprintf(fake[n_fake++], sizeof fake[0], "%s", nick);
}

/* Reservar = entrar logo na lista (aqui não há a janela entre o OK e o join
 * do servidor; essa testa-se no test_roster e no probe_m3.py). */
static int fake_claim(void *ctx, const char *nick)
{
    (void)ctx;
    for (size_t i = 0; i < n_fake; i++)
        if (proto_nick_eq(fake[i], nick))
            return 0;
    fake_add(nick);
    return 1;
}

static int fake_lookup(void *ctx, const char *nick, char canon[PROTO_NICK_MAX + 1])
{
    (void)ctx;
    for (size_t i = 0; i < n_fake; i++)
        if (proto_nick_eq(fake[i], nick)) {
            memcpy(canon, fake[i], PROTO_NICK_MAX + 1);
            return 1;
        }
    return 0;
}

static size_t fake_nicks(void *ctx, char (*out)[PROTO_NICK_MAX + 1], size_t cap)
{
    (void)ctx;
    size_t n = 0;
    for (; n < n_fake && n < cap; n++)
        memcpy(out[n], fake[n], PROTO_NICK_MAX + 1);
    return n;
}

static const struct sess_dir DIR = { NULL, fake_claim, fake_lookup, fake_nicks };

/* session_init com o diretório falso, VAZIO: cada caso começa sem ninguém. */
static void start(struct session *x)
{
    fake_reset();
    session_init(x, &DIR);
}

/* ------------------------------------------------------------------------ */

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
    start(&s);
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
    start(&s);
    CHECK_INT(s.state, SESS_AWAITING_NICK);
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}"), OK_LINE);
    CHECK_INT(s.state, SESS_READY);
    CHECK_STR(s.nick, "ana");
}

/* 34. MSG em READY -> CHAT com o nick DA SESSAO. */
static void caso_34_msg_vira_chat(void)
{
    CASE(34, "MSG -> CHAT com from = nick da sessao");
    start(&s);
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
    start(&s);
    CHECK_STR(send_line("nao e json"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"invalid JSON\"}\n");
    CHECK_INT(s.state, SESS_AWAITING_NICK);

    CASE(35, "JSON invalido em READY -> 400, continua READY com o mesmo nick");
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    CHECK_STR(send_line("{\"type\":\"MSG\"}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"invalid field\"}\n");
    CHECK_INT(s.state, SESS_READY);
    CHECK_STR(s.nick, "ana");

    /* No M1/M2 o exemplo aqui era LIST; desde o M3 o LIST existe. */
    CASE(35, "tipo desconhecido -> 400 unknown type");
    CHECK_STR(send_line("{\"type\":\"XPTO\"}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"unknown type\"}\n");
}

/* 36. Segundo NICK -> 400; o nick NAO muda. */
static void caso_36_segundo_nick(void)
{
    CASE(36, "NICK depois de registado -> 400 nick already set");
    start(&s);
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
    start(&s);
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
    start(&s);
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
    start(&s);
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
    start(&s);
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
    start(&s);
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
    start(&s);
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
    start(&s);
    send_line("{\"type\":\"NICK\",\"nick\":\"ana\"}");
    CHECK_INT(session_leave(&s, &m), 1);
    CHECK_INT(m.type, MT_EVENT);
    CHECK_INT(proto_encode(&m, wire, sizeof wire, NULL), 0);
    CHECK_STR(wire, "{\"type\":\"EVENT\",\"text\":\"ana left\"}\n");

    CASE(51, "quem sai ANTES do NICK nao gera EVENT: ninguem sabia que estava la");
    start(&s);
    CHECK_INT(session_leave(&s, &m), 0);

    CASE(51, "um NICK recusado tambem nao conta como entrada");
    start(&s);
    send_line("{\"type\":\"NICK\",\"nick\":\"a b\"}");      /* espaco: invalido */
    CHECK_INT(session_leave(&s, &m), 0);
}

/* 52. Um nick no limite (32 bytes) ainda produz EVENT validos e codificaveis.
 *     O _Static_assert do session.c garante que cabe; isto executa-o. */
static void caso_52_nick_no_limite(void)
{
    const char *n32 = "12345678901234567890123456789012";   /* 32 */

    CASE(52, "nick de 32 bytes: EVENT de entrada cabe e codifica");
    start(&s);
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


/* ---- M3 ----------------------------------------------------------------- */

#define NICK_ANA "{\"type\":\"NICK\",\"nick\":\"ana\"}"

/* 63. Nick em uso -> 409 (slide 124), e pode-se tentar outro (D19). */
static void caso_63_nick_em_uso(void)
{
    CASE(63, "NICK de alguem que ja esta -> ERROR 409 nick taken (slide 124)");
    start(&s);
    fake_add("ana");
    CHECK_STR(send_line(NICK_ANA),
              "{\"type\":\"ERROR\",\"code\":409,\"reason\":\"nick taken\"}\n");
    CHECK_INT(n_outs, 1);
    CHECK_INT(outs[0].dest, SESS_SELF);
    CHECK_INT(s.state, SESS_AWAITING_NICK);
    CHECK_STR(s.nick, "");

    CASE(63, "sem olhar a maiusculas (D18): \"ANA\" tambem e 409");
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ANA\"}"),
              "{\"type\":\"ERROR\",\"code\":409,\"reason\":\"nick taken\"}\n");

    CASE(63, "a ligacao continua: outro nick entra (D19)");
    CHECK_STR(send_line("{\"type\":\"NICK\",\"nick\":\"ana2\"}"), OK_LINE);
    CHECK_INT(s.state, SESS_READY);
    CHECK_STR(s.nick, "ana2");

    CASE(63, "um 409 nao conta como entrada: saindo logo, nao ha EVENT");
    struct msg m;
    start(&s);
    fake_add("ana");
    send_line(NICK_ANA);
    CHECK_INT(session_leave(&s, &m), 0);
}

/* 64. PRIV -> CHAT privado ao destino + copia ao emissor (D14). */
static void caso_64_priv(void)
{
    const char *chat = "{\"type\":\"CHAT\",\"from\":\"ana\",\"to\":\"rui\","
                       "\"text\":\"hi\",\"private\":true}\n";

    CASE(64, "PRIV -> 2 saidas: TO (destino) e depois SELF (copia)");
    start(&s);
    fake_add("rui");
    send_line(NICK_ANA);
    CHECK_STR(send_line("{\"type\":\"PRIV\",\"to\":\"rui\",\"text\":\"hi\"}"), chat);
    CHECK_INT(n_outs, 2);
    CHECK_INT(outs[0].dest, SESS_TO);
    CHECK_STR(outs[0].msg.to, "rui");
    CHECK_INT(outs[1].dest, SESS_SELF);
    CHECK_STR(wire_of(1), chat);

    CASE(64, "o from vem da sessao, nunca do cliente");
    send_line("{\"type\":\"PRIV\",\"to\":\"rui\",\"text\":\"hi\",\"from\":\"zed\"}");
    CHECK_STR(outs[0].msg.from, "ana");

    CASE(64, "o to sai como foi REGISTADO: /msg RUI chega como \"to\":\"rui\" (D18)");
    send_line("{\"type\":\"PRIV\",\"to\":\"RUI\",\"text\":\"hi\"}");
    CHECK_INT(n_outs, 2);
    CHECK_STR(outs[0].msg.to, "rui");
}

/* 65. PRIV para quem nao existe -> 404 (D15); antes do NICK -> 401. */
static void caso_65_priv_erros(void)
{
    CASE(65, "PRIV para nick que nao esta -> ERROR 404 no such nick");
    start(&s);
    send_line(NICK_ANA);
    CHECK_STR(send_line("{\"type\":\"PRIV\",\"to\":\"ninguem\",\"text\":\"hi\"}"),
              "{\"type\":\"ERROR\",\"code\":404,\"reason\":\"no such nick\"}\n");
    CHECK_INT(n_outs, 1);
    CHECK_INT(outs[0].dest, SESS_SELF);
    CHECK_INT(s.state, SESS_READY);

    CASE(65, "PRIV antes do NICK -> 401 (mesmo que o destino exista)");
    start(&s);
    fake_add("rui");
    CHECK_STR(send_line("{\"type\":\"PRIV\",\"to\":\"rui\",\"text\":\"hi\"}"),
              "{\"type\":\"ERROR\",\"code\":401,\"reason\":\"nick required\"}\n");

    CASE(65, "PRIV sem text -> 400 invalid field");
    start(&s);
    send_line(NICK_ANA);
    CHECK_STR(send_line("{\"type\":\"PRIV\",\"to\":\"ana\"}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"invalid field\"}\n");
}

/* 66. PRIV a si proprio: entregue UMA vez. */
static void caso_66_priv_a_si_proprio(void)
{
    CASE(66, "PRIV para o proprio -> 1 saida, SELF");
    start(&s);
    send_line(NICK_ANA);
    CHECK_STR(send_line("{\"type\":\"PRIV\",\"to\":\"Ana\",\"text\":\"nota\"}"),
              "{\"type\":\"CHAT\",\"from\":\"ana\",\"to\":\"ana\","
              "\"text\":\"nota\",\"private\":true}\n");
    CHECK_INT(n_outs, 1);
    CHECK_INT(outs[0].dest, SESS_SELF);
}

/* 67. LIST -> USERS: inclui o proprio, por ordem alfabetica (D16). */
static void caso_67_list(void)
{
    CASE(67, "LIST -> USERS ordenado, com o proprio, so para quem pediu");
    start(&s);
    fake_add("zed");
    fake_add("Rui");
    send_line(NICK_ANA);                     /* o fake poe a ana na lista */
    CHECK_STR(send_line("{\"type\":\"LIST\"}"),
              "{\"type\":\"USERS\",\"nicks\":[\"ana\",\"Rui\",\"zed\"]}\n");
    CHECK_INT(n_outs, 1);
    CHECK_INT(outs[0].dest, SESS_SELF);

    CASE(67, "sozinho: so o proprio");
    start(&s);
    send_line(NICK_ANA);
    CHECK_STR(send_line("{\"type\":\"LIST\"}"),
              "{\"type\":\"USERS\",\"nicks\":[\"ana\"]}\n");

    CASE(67, "LIST antes do NICK -> 401");
    start(&s);
    CHECK_STR(send_line("{\"type\":\"LIST\"}"),
              "{\"type\":\"ERROR\",\"code\":401,\"reason\":\"nick required\"}\n");
}

/* 68. LIST com nicks no limite (32 bytes): o USERS codifica. (O pior caso,
 *     64 nicks, e o test_protocol #59; aqui e o caminho da session.) */
static void caso_68_list_cheia(void)
{
    CASE(68, "USERS com nicks no limite codifica e cabe numa linha");
    start(&s);
    char nk[PROTO_NICK_MAX + 1];
    for (int i = 0; i < FAKE_MAX - 1; i++) {
        memset(nk, 'a' + i, PROTO_NICK_MAX);
        nk[PROTO_NICK_MAX] = '\0';
        fake_add(nk);
    }
    send_line(NICK_ANA);
    CHECK(strncmp(send_line("{\"type\":\"LIST\"}"), "{\"type\":\"USERS\"", 15) == 0);
    CHECK_INT(outs[0].msg.n_nicks, FAKE_MAX);
}

/* 69. QUIT -> OK e CLOSED (D17); o EVENT de saida vem do session_leave. */
static void caso_69_quit(void)
{
    struct msg m;

    CASE(69, "QUIT -> OK, so para o proprio, e CLOSED");
    start(&s);
    send_line(NICK_ANA);
    CHECK_STR(send_line("{\"type\":\"QUIT\"}"), OK_LINE);
    CHECK_INT(n_outs, 1);
    CHECK_INT(outs[0].dest, SESS_SELF);
    CHECK_INT(s.state, SESS_CLOSED);

    CASE(69, "depois do QUIT nao se responde a mais nada");
    CHECK_STR(send_line("{\"type\":\"MSG\",\"text\":\"ainda?\"}"), "<SEM SAIDAS>");

    CASE(69, "e a saida e anunciada como numa queda: \"ana left\"");
    CHECK_INT(session_leave(&s, &m), 1);
    CHECK_INT(proto_encode(&m, wire, sizeof wire, NULL), 0);
    CHECK_STR(wire, "{\"type\":\"EVENT\",\"text\":\"ana left\"}\n");

    CASE(69, "QUIT ANTES do NICK tambem funciona: OK, CLOSED, sem EVENT");
    start(&s);
    CHECK_STR(send_line("{\"type\":\"QUIT\"}"), OK_LINE);
    CHECK_INT(s.state, SESS_CLOSED);
    CHECK_INT(session_leave(&s, &m), 0);
}

/* 70. USERS vindo de um cliente: tipo S->C -> 400 unexpected. */
static void caso_70_users_do_cliente(void)
{
    CASE(70, "USERS enviado por um cliente -> 400 unexpected type");
    start(&s);
    CHECK_STR(send_line("{\"type\":\"USERS\",\"nicks\":[]}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"unexpected type\"}\n");
    send_line(NICK_ANA);
    CHECK_STR(send_line("{\"type\":\"USERS\",\"nicks\":[\"x\"]}"),
              "{\"type\":\"ERROR\",\"code\":400,\"reason\":\"unexpected type\"}\n");
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
    caso_63_nick_em_uso();
    caso_64_priv();
    caso_65_priv_erros();
    caso_66_priv_a_si_proprio();
    caso_67_list();
    caso_68_list_cheia();
    caso_69_quit();
    caso_70_users_do_cliente();
    return t_report("session");
}
