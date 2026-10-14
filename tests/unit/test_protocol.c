/*
 * test_protocol.c — testes da camada de protocolo (linhas <-> struct msg).
 *
 * Sem rede, como os do framing. Cada caso é uma linha tal como o framing a
 * entrega (já sem o '\n') e o que o parser tem de fazer com ela; ou uma
 * struct msg e os bytes exatos que o encoder tem de produzir.
 *
 * Numeração: 11-18 vêm da tabela em docs/milestones/M1.md (o #17, "MSG antes
 * de NICK", é da máquina de estados do servidor e é testado com ela);
 * 25-31 foram acrescentados ao desenhar o contrato; 32 (e a 2.a verificacao
 * do 12) nasceram de testes de mutacao. 33 e do M2 (EVENT); 58-62 do M3
 * (PRIV, LIST, QUIT, USERS, CHAT.to, regra de nicks, comparacao de nicks).
 */
#include "protocol.h"
#include "test_util.h"

static struct msg       m;
static struct proto_err err;
static char             out[PROTO_LINE_BUF];
static char             big[PROTO_LINE_BUF];

/* Comprimento da linha SEM o '\n', para voltar a dar ao parse o que o encode
 * produziu. Se o encode falhou (len == 0), `len - 1` dava a volta para
 * SIZE_MAX e o teste rebentava em vez de reportar — um teste tem de falhar
 * com um relatorio, nunca com um segfault. */
static size_t sem_nl(size_t len) { return len ? len - 1 : 0; }

/* Conveniência para literais: len = strlen. */
static int parse(const char *s)
{
    return proto_parse(s, strlen(s), &m, &err);
}

/* Constrói `{"type":"<type>","<key>":"<n x ch>"}` em big[] — para os limites. */
static const char *json_with(const char *type, const char *key, char ch, size_t n)
{
    int k = snprintf(big, sizeof big, "{\"type\":\"%s\",\"%s\":\"", type, key);
    memset(big + k, ch, n);
    strcpy(big + k + n, "\"}");
    return big;
}

/* ------------------------------------------------------------------------ */

/* 11. O caminho feliz dos dois tipos C->S do M1. */
static void caso_11_validos(void)
{
    CASE(11, "NICK valido");
    CHECK_INT(parse("{\"type\":\"NICK\",\"nick\":\"ana\"}"), 0);
    CHECK_INT(m.type, MT_NICK);
    CHECK_STR(m.nick, "ana");

    CASE(11, "MSG valido");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":\"ola\"}"), 0);
    CHECK_INT(m.type, MT_MSG);
    CHECK_STR(m.text, "ola");

    CASE(11, "a ordem das chaves nao importa");
    CHECK_INT(parse("{\"nick\":\"rui\",\"type\":\"NICK\"}"), 0);
    CHECK_STR(m.nick, "rui");
}

/* 12. Campo obrigatorio em falta. O bug classico e item->valuestring sem
 *     verificar: e NULL para campos ausentes -> segfault. Aqui tem de ser 400. */
static void caso_12_campo_em_falta(void)
{
    CASE(12, "MSG sem text -> 400, sem segfault");
    CHECK_INT(parse("{\"type\":\"MSG\"}"), -1);
    CHECK_INT(err.code, 400);
    CHECK_STR(err.reason, PROTO_R_FIELD);

    /* Aqui o parser ja tinha reconhecido o tipo (MSG) antes de falhar no
     * text: e o caminho em que podiam ficar restos em *out. */
    CASE(12, "depois de falhar a meio, *out fica a zeros");
    CHECK_INT(m.type, 0);

    CASE(12, "NICK sem nick -> 400");
    CHECK_INT(parse("{\"type\":\"NICK\"}"), -1);
    CHECK_INT(err.code, 400);

    CASE(12, "sem type -> 400");
    CHECK_INT(parse("{\"nick\":\"ana\"}"), -1);
    CHECK_INT(err.code, 400);
    CHECK_STR(err.reason, PROTO_R_FIELD);
}

/* 13. Campo presente mas do tipo errado. */
static void caso_13_tipo_errado(void)
{
    CASE(13, "text numerico -> 400");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":123}"), -1);
    CHECK_INT(err.code, 400);

    CASE(13, "nick null -> 400");
    CHECK_INT(parse("{\"type\":\"NICK\",\"nick\":null}"), -1);
    CHECK_INT(err.code, 400);

    CASE(13, "type numerico -> 400");
    CHECK_INT(parse("{\"type\":5}"), -1);
    CHECK_INT(err.code, 400);

    CASE(13, "text array -> 400");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":[\"a\"]}"), -1);
    CHECK_INT(err.code, 400);
}

/* 14. Nao e JSON. */
static void caso_14_nao_json(void)
{
    CASE(14, "texto solto -> 400 invalid JSON");
    CHECK_INT(parse("nao e json"), -1);
    CHECK_INT(err.code, 400);
    CHECK_STR(err.reason, PROTO_R_JSON);

    CASE(14, "objeto por fechar -> 400");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":\"ola\""), -1);
    CHECK_STR(err.reason, PROTO_R_JSON);

    CASE(14, "linha vazia -> 400");
    CHECK_INT(proto_parse("", 0, &m, &err), -1);
    CHECK_STR(err.reason, PROTO_R_JSON);
}

/* 15. Tipo desconhecido: string valida, mas nao e um tipo do M1. */
static void caso_15_tipo_desconhecido(void)
{
    CASE(15, "XPTO -> 400 unknown type");
    CHECK_INT(parse("{\"type\":\"XPTO\"}"), -1);
    CHECK_INT(err.code, 400);
    CHECK_STR(err.reason, PROTO_R_TYPE);

    CASE(15, "o type e case-sensitive: \"nick\" != \"NICK\"");
    CHECK_INT(parse("{\"type\":\"nick\",\"nick\":\"ana\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_TYPE);

    /* No M1/M2 aqui estava LIST (ainda nao existia). Desde o M3 todos os
     * tipos do slide 124 existem; fica o case-sensitive dos novos. */
    CASE(15, "\"list\" em minusculas nao e LIST");
    CHECK_INT(parse("{\"type\":\"list\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_TYPE);
}

/* 16. Campos extra sao IGNORADOS (PROTOCOL.md §3) — e o que permite a outros
 *     grupos acrescentar coisas sem partir o nosso servidor. */
static void caso_16_campos_extra(void)
{
    CASE(16, "campo extra ignorado");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":\"oi\",\"zz\":1}"), 0);
    CHECK_STR(m.text, "oi");

    CASE(16, "campo extra aninhado ignorado");
    CHECK_INT(parse("{\"type\":\"NICK\",\"meta\":{\"v\":[1,2]},\"nick\":\"ana\"}"), 0);
    CHECK_STR(m.nick, "ana");
}

/* 18. Aspas e barras no texto: escapadas na saida, recuperadas na entrada. */
static void caso_18_escapes(void)
{
    CASE(18, "\" e \\ escapados no encode");
    struct msg c = { .type = MT_CHAT, .from = "ana", .text = "diz \"oi\" \\ adeus" };
    size_t len = 0;
    CHECK_INT(proto_encode(&c, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"CHAT\",\"from\":\"ana\","
                   "\"text\":\"diz \\\"oi\\\" \\\\ adeus\",\"private\":false}\n");
    CHECK_INT(len, strlen(out));

    CASE(18, "e recuperados no parse");
    CHECK_INT(proto_parse(out, sem_nl(len), &m, &err), 0);
    CHECK_STR(m.text, "diz \"oi\" \\ adeus");
}

/* 25. Lixo depois do objeto. cJSON_Parse le o PRIMEIRO objeto e ignora o
 *     resto em silencio — "{...}{...}" perderia a 2.a mensagem sem erro. */
static void caso_25_lixo_a_seguir(void)
{
    CASE(25, "dois objetos na mesma linha -> 400");
    CHECK_INT(parse("{\"type\":\"OK\"}{\"type\":\"OK\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_JSON);

    CASE(25, "lixo depois do objeto -> 400");
    CHECK_INT(parse("{\"type\":\"OK\"} xyz"), -1);

    CASE(25, "espacos antes e depois sao aceites");
    CHECK_INT(parse("  {\"type\":\"OK\"} \t "), 0);
    CHECK_INT(m.type, MT_OK);
}

/* 26. Limites e regras de conteudo (D4). Os dois lados de cada fronteira. */
static void caso_26_limites(void)
{
    CASE(26, "nick com 32 bytes aceite");
    CHECK_INT(parse(json_with("NICK", "nick", 'n', PROTO_NICK_MAX)), 0);
    CHECK_INT(strlen(m.nick), PROTO_NICK_MAX);

    CASE(26, "nick com 33 bytes -> 400");
    CHECK_INT(parse(json_with("NICK", "nick", 'n', PROTO_NICK_MAX + 1)), -1);
    CHECK_INT(err.code, 400);

    CASE(26, "nick vazio -> 400");
    CHECK_INT(parse("{\"type\":\"NICK\",\"nick\":\"\"}"), -1);

    CASE(26, "nick com espaco -> 400");
    CHECK_INT(parse("{\"type\":\"NICK\",\"nick\":\"a b\"}"), -1);

    CASE(26, "nick com tab (escape JSON) -> 400");
    CHECK_INT(parse("{\"type\":\"NICK\",\"nick\":\"a\\tb\"}"), -1);

    CASE(26, "text com 1024 bytes aceite");
    CHECK_INT(parse(json_with("MSG", "text", 't', PROTO_TEXT_MAX)), 0);
    CHECK_INT(strlen(m.text), PROTO_TEXT_MAX);

    CASE(26, "text com 1025 bytes -> 400");
    CHECK_INT(parse(json_with("MSG", "text", 't', PROTO_TEXT_MAX + 1)), -1);

    CASE(26, "text vazio -> 400");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":\"\"}"), -1);

    CASE(26, "text com \\n (escape JSON) -> 400: sem mensagens multi-linha");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":\"a\\nb\"}"), -1);

    CASE(26, "text UTF-8 conta BYTES: \"ola\" com acento = 4");
    CHECK_INT(parse("{\"type\":\"MSG\",\"text\":\"ol\xC3\xA1\"}"), 0);
    CHECK_INT(strlen(m.text), 4);
}

/* 27. O pior caso do encode, que justifica o _Static_assert do protocol.h:
 *     from/to e text no limite, o text cheio de caracteres que o cJSON escapa
 *     (2x). Desde o M3 (D20) os nicks ja nao podem ter '"' nem '\\', por isso
 *     o from deixou de crescer; em troca o CHAT privado ganhou o "to".
 *     Tem de caber numa linha do framing e ter UM so '\n', no fim. */
static void caso_27_pior_caso_encode(void)
{
    CASE(27, "pior CHAT cabe numa linha e tem um so '\\n'");
    struct msg c = { .type = MT_CHAT, .is_private = 1 };
    memset(c.from, 'f',  PROTO_NICK_MAX);
    memset(c.to,   't',  PROTO_NICK_MAX);
    memset(c.text, '\\', PROTO_TEXT_MAX);
    size_t len = 0;
    CHECK_INT(proto_encode(&c, out, sizeof out, &len), 0);
    CHECK(len <= RX_MAX_LINE);
    CHECK_INT(len, PROTO_CHAT_OVERHEAD + 2 * PROTO_NICK_MAX + 2 * PROTO_TEXT_MAX - 1); /* "true" e 1 mais curto que "false" */
    CHECK(len >= 1 && memchr(out, '\n', sem_nl(len)) == NULL);
    CHECK_INT(out[sem_nl(len)], '\n');

    CASE(27, "e volta intacto pelo parse");
    CHECK_INT(proto_parse(out, sem_nl(len), &m, &err), 0);
    CHECK_INT(m.type, MT_CHAT);
    CHECK_MEM(m.from, c.from, PROTO_NICK_MAX + 1);
    CHECK_MEM(m.to,   c.to,   PROTO_NICK_MAX + 1);
    CHECK_MEM(m.text, c.text, PROTO_TEXT_MAX + 1);
    CHECK_INT(m.is_private, 1);

    CASE(27, "encode recusa o que o parse recusaria");
    struct msg bad = { .type = MT_MSG, .text = "a\nb" };
    CHECK_INT(proto_encode(&bad, out, sizeof out, &len), -1);
    struct msg bad2 = { .type = MT_NICK };                 /* nick vazio */
    CHECK_INT(proto_encode(&bad2, out, sizeof out, &len), -1);
    struct msg bad3 = { .type = 0 };                       /* tipo nenhum */
    CHECK_INT(proto_encode(&bad3, out, sizeof out, &len), -1);
    struct msg bad4 = { .type = MT_ERROR, .code = 40 };    /* code fora de 100..999 */
    CHECK_INT(proto_encode(&bad4, out, sizeof out, &len), -1);

    CASE(27, "encode recusa out NULL ou cap pequeno (culpa nossa)");
    struct msg ok = { .type = MT_OK };
    CHECK_INT(proto_encode(&ok, NULL, sizeof out, &len), -1);
    CHECK_INT(proto_encode(&ok, out, PROTO_LINE_BUF - 1, &len), -1);
}

/* 28. JSON valido mas nao e um objeto. */
static void caso_28_raiz_nao_objeto(void)
{
    CASE(28, "array, string, null e numero na raiz -> 400");
    CHECK_INT(parse("[1,2]"), -1);
    CHECK_STR(err.reason, PROTO_R_JSON);
    CHECK_INT(parse("\"NICK\""), -1);
    CHECK_INT(parse("null"), -1);
    CHECK_INT(parse("42"), -1);
}

/* 29. '\0' embebido na linha (o framing deixa passar — e o caso #20 de la).
 *     Sem esta verificacao o cJSON pararia no '\0' e leria so metade. */
static void caso_29_nul_embebido(void)
{
    CASE(29, "'\\0' dentro da linha -> 400");
    const char in[] = "{\"type\":\"MSG\",\"text\":\"a\0b\"}";
    CHECK_INT(proto_parse(in, sizeof in - 1, &m, &err), -1);
    CHECK_STR(err.reason, PROTO_R_JSON);
}

/* 30. Tipos S->C: e o que o NOSSO cliente vai ler dos servidores dos outros. */
static void caso_30_servidor_para_cliente(void)
{
    CASE(30, "OK");
    CHECK_INT(parse("{\"type\":\"OK\"}"), 0);
    CHECK_INT(m.type, MT_OK);

    CASE(30, "CHAT com private true");
    CHECK_INT(parse("{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"oi\",\"private\":true}"), 0);
    CHECK_STR(m.from, "ana");
    CHECK_STR(m.text, "oi");
    CHECK_INT(m.is_private, 1);

    CASE(30, "CHAT sem private -> 0 por omissao");
    CHECK_INT(parse("{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"oi\"}"), 0);
    CHECK_INT(m.is_private, 0);

    CASE(30, "CHAT com private nao-booleano -> 400");
    CHECK_INT(parse("{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"oi\",\"private\":\"yes\"}"), -1);

    CASE(30, "ERROR com code e reason (exemplo do slide)");
    CHECK_INT(parse("{\"type\":\"ERROR\",\"code\":409,\"reason\":\"nick taken\"}"), 0);
    CHECK_INT(m.code, 409);
    CHECK_STR(m.reason, "nick taken");

    CASE(30, "ERROR sem reason -> \"\" por omissao");
    CHECK_INT(parse("{\"type\":\"ERROR\",\"code\":400}"), 0);
    CHECK_STR(m.reason, "");

    CASE(30, "ERROR com code string, fracionario ou em falta -> 400");
    CHECK_INT(parse("{\"type\":\"ERROR\",\"code\":\"409\"}"), -1);
    CHECK_INT(parse("{\"type\":\"ERROR\",\"code\":409.5}"), -1);
    CHECK_INT(parse("{\"type\":\"ERROR\",\"reason\":\"x\"}"), -1);
}

/* 31. Os bytes EXATOS dos exemplos do slide 124. E o teste de
 *     interoperabilidade mais barato que existe: se o nosso encode produz o
 *     mesmo que o slide, qualquer grupo que siga o slide le-nos. */
static void caso_31_exemplos_do_slide(void)
{
    size_t len = 0;

    CASE(31, "NICK igual ao slide");
    struct msg n = { .type = MT_NICK, .nick = "ana" };
    CHECK_INT(proto_encode(&n, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"NICK\",\"nick\":\"ana\"}\n");

    CASE(31, "MSG igual ao slide");
    struct msg g = { .type = MT_MSG, .text = "hello" };
    CHECK_INT(proto_encode(&g, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"MSG\",\"text\":\"hello\"}\n");

    CASE(31, "OK igual ao slide");
    struct msg o = { .type = MT_OK };
    CHECK_INT(proto_encode(&o, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"OK\"}\n");
    CHECK_INT(len, 14);

    CASE(31, "CHAT igual ao slide");
    struct msg c = { .type = MT_CHAT, .from = "ana", .text = "hello" };
    CHECK_INT(proto_encode(&c, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"hello\",\"private\":false}\n");

    CASE(31, "ERROR igual ao slide");
    struct msg e = { .type = MT_ERROR, .code = 409, .reason = "nick taken" };
    CHECK_INT(proto_encode(&e, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"ERROR\",\"code\":409,\"reason\":\"nick taken\"}\n");

    CASE(31, "proto_type_name");
    CHECK_STR(proto_type_name(MT_NICK), "NICK");
    CHECK_STR(proto_type_name(MT_ERROR), "ERROR");
    CHECK_STR(proto_type_name(0), "?");
}

/* 32. As CHAVES tambem sao case-sensitive (em JSON sao sempre). "Type" nao e
 *     "type": um parser que as confundisse aceitava o que outro grupo
 *     recusa — e a divergencia so aparecia na interoperabilidade. */
static void caso_32_chaves_case_sensitive(void)
{
    CASE(32, "\"Type\" nao e \"type\" -> 400");
    CHECK_INT(parse("{\"Type\":\"NICK\",\"nick\":\"ana\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_FIELD);

    CASE(32, "\"Text\" nao e \"text\" -> 400");
    CHECK_INT(parse("{\"type\":\"MSG\",\"Text\":\"oi\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_FIELD);
}

/* 33. EVENT — entradas e saidas (M2).
 *
 *     Desceu do M3 para o M2 porque o slide 123 poe "join and leave notices"
 *     no M2. So tem "text", e o exemplo do slide 124 e
 *     {"type":"EVENT","text":"rui joined"}.
 *
 *     Vale a pena testar o parse E o encode: o parse porque um servidor de
 *     outro grupo vai mandar-nos EVENT e o nosso cliente tem de os aceitar;
 *     o encode porque somos nos a produzi-los no broadcast. */
static void caso_33_event(void)
{
    size_t len;

    CASE(33, "parse de um EVENT do slide 124");
    CHECK_INT(parse("{\"type\":\"EVENT\",\"text\":\"rui joined\"}"), 0);
    CHECK_INT(m.type, MT_EVENT);
    CHECK_STR(m.text, "rui joined");

    CASE(33, "encode de um EVENT da os bytes exactos do slide");
    struct msg e = { .type = MT_EVENT };
    memcpy(e.text, "rui joined", sizeof "rui joined");
    CHECK_INT(proto_encode(&e, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"EVENT\",\"text\":\"rui joined\"}\n");

    CASE(33, "round-trip: o que codificamos volta a dar parse igual");
    CHECK_INT(proto_parse(out, sem_nl(len), &m, &err), 0);
    CHECK_INT(m.type, MT_EVENT);
    CHECK_STR(m.text, "rui joined");

    CASE(33, "EVENT sem \"text\" -> 400");
    CHECK_INT(parse("{\"type\":\"EVENT\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_FIELD);

    CASE(33, "EVENT com \"text\" vazio -> 400 (text tem minimo 1)");
    CHECK_INT(parse("{\"type\":\"EVENT\",\"text\":\"\"}"), -1);

    CASE(33, "EVENT com '\\n' no texto -> 400: partiria a linha no receptor");
    CHECK_INT(parse("{\"type\":\"EVENT\",\"text\":\"a\\nb\"}"), -1);

    CASE(33, "\"event\" em minusculas nao e EVENT");
    CHECK_INT(parse("{\"type\":\"event\",\"text\":\"x\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_TYPE);

    CASE(33, "proto_type_name(MT_EVENT)");
    CHECK_STR(proto_type_name(MT_EVENT), "EVENT");

    CASE(33, "text no limite (1024) ainda cabe numa linha");
    CHECK_INT(parse(json_with("EVENT", "text", 'e', PROTO_TEXT_MAX)), 0);
    CHECK_INT(proto_encode(&m, out, sizeof out, &len), 0);
    CHECK_INT(parse(json_with("EVENT", "text", 'e', PROTO_TEXT_MAX + 1)), -1);
}


/* 58. Os tres pedidos novos do M3 (slide 124): PRIV, LIST, QUIT. */
static void caso_58_pedidos_m3(void)
{
    size_t len;

    CASE(58, "PRIV do slide 124");
    CHECK_INT(parse("{\"type\":\"PRIV\",\"to\":\"rui\",\"text\":\"hi\"}"), 0);
    CHECK_INT(m.type, MT_PRIV);
    CHECK_STR(m.to, "rui");
    CHECK_STR(m.text, "hi");

    CASE(58, "PRIV sem to, sem text, ou com to invalido -> 400 invalid field");
    CHECK_INT(parse("{\"type\":\"PRIV\",\"text\":\"hi\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_FIELD);
    CHECK_INT(parse("{\"type\":\"PRIV\",\"to\":\"rui\"}"), -1);
    CHECK_INT(parse("{\"type\":\"PRIV\",\"to\":\"a b\",\"text\":\"hi\"}"), -1);
    CHECK_INT(parse("{\"type\":\"PRIV\",\"to\":\"\",\"text\":\"hi\"}"), -1);
    CHECK_INT(parse("{\"type\":\"PRIV\",\"to\":7,\"text\":\"hi\"}"), -1);

    CASE(58, "LIST e QUIT nao tem campos (e campos extra sao ignorados)");
    CHECK_INT(parse("{\"type\":\"LIST\"}"), 0);
    CHECK_INT(m.type, MT_LIST);
    CHECK_INT(parse("{\"type\":\"QUIT\",\"bye\":true}"), 0);
    CHECK_INT(m.type, MT_QUIT);

    CASE(58, "encode: os bytes exatos do slide 124");
    struct msg p = { .type = MT_PRIV, .to = "rui", .text = "hi" };
    CHECK_INT(proto_encode(&p, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"PRIV\",\"to\":\"rui\",\"text\":\"hi\"}\n");
    struct msg l = { .type = MT_LIST };
    CHECK_INT(proto_encode(&l, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"LIST\"}\n");
    struct msg q = { .type = MT_QUIT };
    CHECK_INT(proto_encode(&q, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"QUIT\"}\n");

    CASE(58, "encode recusa PRIV sem destino");
    struct msg bad = { .type = MT_PRIV, .text = "hi" };
    CHECK_INT(proto_encode(&bad, out, sizeof out, &len), -1);

    CASE(58, "proto_type_name dos novos");
    CHECK_STR(proto_type_name(MT_PRIV), "PRIV");
    CHECK_STR(proto_type_name(MT_LIST), "LIST");
    CHECK_STR(proto_type_name(MT_QUIT), "QUIT");
    CHECK_STR(proto_type_name(MT_USERS), "USERS");
}

/* 59. USERS: o primeiro array do protocolo. E o pior caso, que justifica o
 *     D20 (nicks sem '"' nem '\'). */
static void caso_59_users(void)
{
    size_t len;

    CASE(59, "USERS do slide 124");
    CHECK_INT(parse("{\"type\":\"USERS\",\"nicks\":[\"ana\",\"rui\"]}"), 0);
    CHECK_INT(m.type, MT_USERS);
    CHECK_INT(m.n_nicks, 2);
    CHECK_STR(m.nicks, "ana rui");

    CASE(59, "lista vazia e aceite");
    CHECK_INT(parse("{\"type\":\"USERS\",\"nicks\":[]}"), 0);
    CHECK_INT(m.n_nicks, 0);
    CHECK_STR(m.nicks, "");

    CASE(59, "nicks em falta, nao-array, elemento nao-string ou invalido -> 400");
    CHECK_INT(parse("{\"type\":\"USERS\"}"), -1);
    CHECK_STR(err.reason, PROTO_R_FIELD);
    CHECK_INT(parse("{\"type\":\"USERS\",\"nicks\":\"ana rui\"}"), -1);
    CHECK_INT(parse("{\"type\":\"USERS\",\"nicks\":[\"ana\",3]}"), -1);
    CHECK_INT(parse("{\"type\":\"USERS\",\"nicks\":[\"ana\",\"a b\"]}"), -1);
    CHECK_INT(parse("{\"type\":\"USERS\",\"nicks\":[\"\"]}"), -1);

    CASE(59, "encode igual ao slide, construido com proto_users_add");
    struct msg u = { .type = MT_USERS };
    CHECK_INT(proto_users_add(&u, "ana"), 0);
    CHECK_INT(proto_users_add(&u, "rui"), 0);
    CHECK_INT(proto_encode(&u, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"USERS\",\"nicks\":[\"ana\",\"rui\"]}\n");

    CASE(59, "e volta igual pelo parse");
    CHECK_INT(proto_parse(out, sem_nl(len), &m, &err), 0);
    CHECK_STR(m.nicks, "ana rui");
    CHECK_INT(m.n_nicks, 2);

    CASE(59, "USERS vazio codifica");
    struct msg v = { .type = MT_USERS };
    CHECK_INT(proto_encode(&v, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"USERS\",\"nicks\":[]}\n");

    CASE(59, "proto_users_add recusa nick invalido e o 65.o");
    struct msg w = { .type = MT_USERS };
    CHECK_INT(proto_users_add(&w, "a b"), -1);
    CHECK_INT(proto_users_add(&w, "a\"b"), -1);
    char nk[PROTO_NICK_MAX + 1];
    for (int i = 0; i < PROTO_USERS_MAX; i++) {
        memset(nk, 'a' + i % 26, PROTO_NICK_MAX);    /* 32 bytes cada */
        nk[PROTO_NICK_MAX] = '\0';
        nk[0] = (char)('A' + i / 26);                /* para nao serem iguais */
        CHECK_INT(proto_users_add(&w, nk), 0);
    }
    CHECK_INT(proto_users_add(&w, "mais1"), -1);
    CHECK_INT(w.n_nicks, PROTO_USERS_MAX);

    CASE(59, "pior USERS (64 nicks de 32 bytes) cabe numa linha");
    CHECK_INT(proto_encode(&w, out, sizeof out, &len), 0);
    CHECK_INT(len, PROTO_USERS_OVERHEAD + PROTO_USERS_MAX * (PROTO_NICK_MAX + 3) - 1);
    CHECK(len <= RX_MAX_LINE);
    CHECK_INT(proto_parse(out, sem_nl(len), &m, &err), 0);
    CHECK_INT(m.n_nicks, PROTO_USERS_MAX);
    CHECK(strcmp(m.nicks, w.nicks) == 0);

    CASE(59, "encode recusa a string empacotada mal formada");
    struct msg x = { .type = MT_USERS, .nicks = "ana  rui", .n_nicks = 2 };
    CHECK_INT(proto_encode(&x, out, sizeof out, &len), -1);
    struct msg y = { .type = MT_USERS, .nicks = "ana rui ", .n_nicks = 2 };
    CHECK_INT(proto_encode(&y, out, sizeof out, &len), -1);
    struct msg z = { .type = MT_USERS, .nicks = "ana rui", .n_nicks = 3 };
    CHECK_INT(proto_encode(&z, out, sizeof out, &len), -1);

    CASE(59, "a receber nao ha limite de contagem: 300 nicks curtos cabem");
    int k = snprintf(big, sizeof big, "{\"type\":\"USERS\",\"nicks\":[");
    for (int i = 0; i < 300; i++)
        k += snprintf(big + k, sizeof big - (size_t)k, "%s\"u%d\"", i ? "," : "", i);
    snprintf(big + k, sizeof big - (size_t)k, "]}");
    CHECK_INT(parse(big), 0);
    CHECK_INT(m.n_nicks, 300);
}

/* 60. CHAT privado com "to" (D14, extensao nossa). */
static void caso_60_chat_to(void)
{
    size_t len;

    CASE(60, "CHAT privado leva \"to\"");
    struct msg c = { .type = MT_CHAT, .from = "ana", .to = "rui", .text = "hi", .is_private = 1 };
    CHECK_INT(proto_encode(&c, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"CHAT\",\"from\":\"ana\",\"to\":\"rui\",\"text\":\"hi\",\"private\":true}\n");
    CHECK_INT(proto_parse(out, sem_nl(len), &m, &err), 0);
    CHECK_STR(m.to, "rui");
    CHECK_INT(m.is_private, 1);

    CASE(60, "CHAT sem \"to\" (o do slide, ou de outro grupo) -> to = \"\"");
    CHECK_INT(parse("{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"hi\",\"private\":true}"), 0);
    CHECK_STR(m.to, "");

    CASE(60, "\"to\" presente mas nao-string -> 400 (como o \"private\")");
    CHECK_INT(parse("{\"type\":\"CHAT\",\"from\":\"ana\",\"text\":\"hi\",\"to\":1}"), -1);
}

/* 61. Regra de nicks (D20): estrita a enviar, larga a receber. */
static void caso_61_regra_de_nicks(void)
{
    CASE(61, "NICK com '\"' ou '\\' -> 400 (D20)");
    CHECK_INT(parse("{\"type\":\"NICK\",\"nick\":\"a\\\"b\"}"), -1);
    CHECK_INT(parse("{\"type\":\"NICK\",\"nick\":\"a\\\\b\"}"), -1);
    CHECK_INT(proto_valid_nick("a\"b"), 0);
    CHECK_INT(proto_valid_nick("a\\b"), 0);

    CASE(61, "outros simbolos e UTF-8 continuam aceites");
    CHECK_INT(proto_valid_nick("ana_99-x.y"), 1);
    CHECK_INT(proto_valid_nick("z\xC3\xA9"), 1);

    CASE(61, "um CHAT de um servidor com aspas no from e aceite (tolerante)");
    CHECK_INT(parse("{\"type\":\"CHAT\",\"from\":\"a\\\"b\",\"text\":\"oi\"}"), 0);
    CHECK_STR(m.from, "a\"b");

    CASE(61, "... e um USERS tambem");
    CHECK_INT(parse("{\"type\":\"USERS\",\"nicks\":[\"a\\\\b\"]}"), 0);
    CHECK_STR(m.nicks, "a\\b");

    CASE(61, "mas espacos no from continuam proibidos: partiriam o USERS e o /msg");
    CHECK_INT(parse("{\"type\":\"CHAT\",\"from\":\"a b\",\"text\":\"oi\"}"), -1);

    CASE(61, "nunca ENVIAMOS o que so aceitamos por tolerancia");
    struct msg c = { .type = MT_CHAT, .from = "a\"b", .text = "oi" };
    size_t len;
    CHECK_INT(proto_encode(&c, out, sizeof out, &len), -1);
}

/* 62. "E o mesmo nick?" (D18): maiusculas ASCII ignoradas, UTF-8 nao. */
static void caso_62_nick_eq(void)
{
    CASE(62, "maiusculas ASCII ignoradas");
    CHECK_INT(proto_nick_eq("ana", "ana"), 1);
    CHECK_INT(proto_nick_eq("Ana", "aNA"), 1);
    CHECK_INT(proto_nick_eq("ana", "ana2"), 0);
    CHECK_INT(proto_nick_eq("ana2", "ana"), 0);
    CHECK_INT(proto_nick_eq("", ""), 1);

    CASE(62, "bytes >= 0x80 comparados tal e qual: Ze com acento != ZE com acento");
    CHECK_INT(proto_nick_eq("z\xC3\xA9", "Z\xC3\xA9"), 1);     /* so o Z muda */
    CHECK_INT(proto_nick_eq("z\xC3\xA9", "z\xC3\x89"), 0);     /* e vs E com acento */

    CASE(62, "simbolos nao sao dobrados: '[' nao e '{'");
    CHECK_INT(proto_nick_eq("a[", "a{"), 0);

    CASE(62, "ordem do USERS: alfabetica sem olhar a maiusculas, empate pelos bytes");
    CHECK(proto_nick_cmp("ana", "Rui") < 0);
    CHECK(proto_nick_cmp("rui", "Ana") > 0);
    CHECK(proto_nick_cmp("ana", "anabela") < 0);
    CHECK(proto_nick_cmp("Ana", "ana") < 0);
    CHECK_INT(proto_nick_cmp("ana", "ana"), 0);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    caso_11_validos();
    caso_12_campo_em_falta();
    caso_13_tipo_errado();
    caso_14_nao_json();
    caso_15_tipo_desconhecido();
    caso_16_campos_extra();
    caso_18_escapes();
    caso_25_lixo_a_seguir();
    caso_26_limites();
    caso_27_pior_caso_encode();
    caso_28_raiz_nao_objeto();
    caso_29_nul_embebido();
    caso_30_servidor_para_cliente();
    caso_31_exemplos_do_slide();
    caso_32_chaves_case_sensitive();
    caso_33_event();
    caso_58_pedidos_m3();
    caso_59_users();
    caso_60_chat_to();
    caso_61_regra_de_nicks();
    caso_62_nick_eq();
    return t_report("protocol");
}
