/*
 * test_disco.c — testes da descoberta (M5): os datagramas, sem rede.
 *
 * Como os do protocol: cada caso é um datagrama tal como o recvfrom() o
 * entrega (bytes e tamanho, sem '\0' no fim garantido) e o que a camada tem
 * de fazer com ele; ou valores e os bytes exatos que tem de produzir.
 *
 * Numeração: continua a do M4 (93). 94 pedidos, 95 respostas que enviamos,
 * 96 respostas que recebemos, 97 nomes, 98 a lista sem repetidos, 99 a linha
 * que o cliente mostra. A tabela está em docs/milestones/M5.md.
 */
#include "disco.h"
#include "test_util.h"

#include <limits.h>

static char                out[DISCO_REPLY_BUF];
static char                big[2 * DISCO_DGRAM_MAX];
static struct disco_server srv;

/* Conveniência para literais: len = strlen. */
static int req(const char *s)
{
    return disco_is_request(s, strlen(s));
}

static int rep(const char *s)
{
    return disco_parse_reply(s, strlen(s), &srv);
}

/* `{"type":"<type>","pad":"aaa..."}` com EXATAMENTE `total` bytes — para o
 * limite do tamanho do datagrama. */
static const char *json_of_len(const char *type, size_t total)
{
    int    k   = snprintf(big, sizeof big, "{\"type\":\"%s\",\"pad\":\"", type);
    size_t pad = total - (size_t)k - 2;     /* 2 = o '"}' do fim */
    memset(big + k, 'a', pad);
    strcpy(big + k + pad, "\"}");
    return big;
}

static struct disco_server mk(const char *ip, int port, const char *name, int users)
{
    struct disco_server s;
    memset(&s, 0, sizeof s);
    snprintf(s.ip, sizeof s.ip, "%s", ip);
    s.tcp_port = port;
    snprintf(s.name, sizeof s.name, "%s", name);
    s.users = users;
    return s;
}

/* ------------------------------------------------------------------------ */

/* 94. O servidor: o que é um pedido (e o que NÃO é, para ficar calado). */
static void caso_94_pedidos(void)
{
    CASE(94, "o pedido do nosso cliente: 19 bytes, e e um pedido");
    CHECK_INT(strlen(DISCO_REQUEST), 19);
    CHECK_INT(req(DISCO_REQUEST), 1);

    CASE(94, "JSON: com '\\n', '\\r\\n', espacos e campos a mais");
    CHECK_INT(req("{\"type\":\"DISCOVER\"}\n"), 1);
    CHECK_INT(req("{\"type\":\"DISCOVER\"}\r\n"), 1);
    CHECK_INT(req("  { \"type\" : \"DISCOVER\" }  "), 1);
    CHECK_INT(req("{\"v\":1,\"type\":\"DISCOVER\",\"from\":\"g3\"}"), 1);

    CASE(94, "texto simples DISCOVER, com espacos a volta");
    CHECK_INT(req("DISCOVER"), 1);
    CHECK_INT(req("DISCOVER\n"), 1);
    CHECK_INT(req(" \tDISCOVER\r\n"), 1);

    CASE(94, "maiusculas contam (S3)");
    CHECK_INT(req("discover"), 0);
    CHECK_INT(req("{\"type\":\"discover\"}"), 0);
    CHECK_INT(req("{\"Type\":\"DISCOVER\"}"), 0);

    CASE(94, "outros tipos NAO sao pedidos: sem ping-pong entre servidores");
    CHECK_INT(req("{\"type\":\"SERVER\",\"tcp_port\":5000}"), 0);
    CHECK_INT(disco_build_reply(5000, "x", 0, out, sizeof out, NULL), 0);
    CHECK_INT(req(out), 0);                  /* a nossa propria resposta */
    CHECK_INT(req("{\"type\":\"NICK\",\"nick\":\"ana\"}"), 0);
    CHECK_INT(req("SERVER"), 0);

    CASE(94, "lixo: silencio");
    CHECK_INT(req(""), 0);
    CHECK_INT(disco_is_request(NULL, 0), 0);
    CHECK_INT(req("\n"), 0);
    CHECK_INT(req("DISCOVERY"), 0);
    CHECK_INT(req("DISCOVER please"), 0);
    CHECK_INT(req("\"DISCOVER\""), 0);       /* JSON, mas nao e objeto */
    CHECK_INT(req("[\"DISCOVER\"]"), 0);
    CHECK_INT(req("{\"type\":\"DISCOVER\""), 0);
    CHECK_INT(req("{}"), 0);
    CHECK_INT(req("{\"type\":1}"), 0);
    CHECK_INT(req("{\"type\":null}"), 0);

    CASE(94, "dois objetos num datagrama: recusado inteiro");
    CHECK_INT(req("{\"type\":\"DISCOVER\"}{\"type\":\"DISCOVER\"}"), 0);

    CASE(94, "'\\0' embebido e UTF-8 invalido");
    CHECK_INT(disco_is_request("DISCOVER\0", 9), 0);
    CHECK_INT(disco_is_request("{\"type\":\"DISCOVER\"}\0", 20), 0);
    CHECK_INT(req("{\"type\":\"DISCOVER\",\"x\":\"\xff\"}"), 0);
    CHECK_INT(req("{\"type\":\"DISCOVER\",\"x\":\"\xc3\x28\"}"), 0);
    CHECK_INT(req("{\"type\":\"DISCOVER\",\"x\":\"Z\xc3\xa9\"}"), 1);  /* "Zé" e valido */

    CASE(94, "tamanho: 512 bytes ainda e pedido, 513 ja nao");
    CHECK_INT(strlen(json_of_len("DISCOVER", DISCO_DGRAM_MAX)), DISCO_DGRAM_MAX);
    CHECK_INT(disco_is_request(big, DISCO_DGRAM_MAX), 1);
    json_of_len("DISCOVER", DISCO_DGRAM_MAX + 1);
    CHECK_INT(disco_is_request(big, DISCO_DGRAM_MAX + 1), 0);
}

/* 95. O servidor: a resposta, byte a byte. */
static void caso_95_resposta(void)
{
    size_t len = 0;

    CASE(95, "o exemplo do disco.h, byte a byte, com '\\n'");
    CHECK_INT(disco_build_reply(5000, "diogolap", 3, out, sizeof out, &len), 0);
    CHECK_STR(out, "{\"type\":\"SERVER\",\"tcp_port\":5000,\"name\":\"diogolap\",\"users\":3}\n");
    CHECK_INT(len, strlen(out));
    CHECK_INT(out[len - 1], '\n');

    CASE(95, "out_len pode ser NULL; users 0; portas nos limites");
    CHECK_INT(disco_build_reply(1, "a", 0, out, sizeof out, NULL), 0);
    CHECK_STR(out, "{\"type\":\"SERVER\",\"tcp_port\":1,\"name\":\"a\",\"users\":0}\n");
    CHECK_INT(disco_build_reply(65535, "a", INT_MAX, out, sizeof out, NULL), 0);
    CHECK_STR(out, "{\"type\":\"SERVER\",\"tcp_port\":65535,\"name\":\"a\",\"users\":2147483647}\n");

    CASE(95, "o name e escapado pelo JSON; UTF-8 vai tal e qual");
    CHECK_INT(disco_build_reply(5000, "a\"b\\c", 1, out, sizeof out, NULL), 0);
    CHECK_STR(out, "{\"type\":\"SERVER\",\"tcp_port\":5000,\"name\":\"a\\\"b\\\\c\",\"users\":1}\n");
    CHECK_INT(disco_build_reply(5000, "Z\xc3\xa9", 1, out, sizeof out, NULL), 0);
    CHECK_STR(out, "{\"type\":\"SERVER\",\"tcp_port\":5000,\"name\":\"Z\xc3\xa9\",\"users\":1}\n");

    CASE(95, "o pior caso do disco.h: exatamente 192 bytes, cabe num datagrama");
    char worst[DISCO_NAME_MAX + 1];
    memset(worst, '"', DISCO_NAME_MAX);
    worst[DISCO_NAME_MAX] = '\0';
    CHECK_INT(disco_build_reply(65535, worst, INT_MAX, out, sizeof out, &len), 0);
    CHECK_INT(len, DISCO_REPLY_OVERHEAD + 2 * DISCO_NAME_MAX);
    CHECK(len <= DISCO_DGRAM_MAX);

    CASE(95, "valores invalidos: -1 e out vazio (nunca meio preenchido)");
    CHECK_INT(disco_build_reply(0, "a", 0, out, sizeof out, NULL), -1);
    CHECK_STR(out, "");
    CHECK_INT(disco_build_reply(65536, "a", 0, out, sizeof out, NULL), -1);
    CHECK_INT(disco_build_reply(-1, "a", 0, out, sizeof out, NULL), -1);
    CHECK_INT(disco_build_reply(5000, "a", -1, out, sizeof out, NULL), -1);
    CHECK_INT(disco_build_reply(5000, "", 0, out, sizeof out, NULL), -1);
    CHECK_INT(disco_build_reply(5000, NULL, 0, out, sizeof out, NULL), -1);
    CHECK_INT(disco_build_reply(5000, "a\nb", 0, out, sizeof out, NULL), -1);

    CASE(95, "cap: tem de caber o '\\n' E o '\\0'");
    CHECK_INT(disco_build_reply(5000, "a", 0, out, sizeof out, &len), 0);
    size_t need = len + 1;
    CHECK_INT(disco_build_reply(5000, "a", 0, out, need - 1, NULL), -1);
    CHECK_STR(out, "");
    CHECK_INT(disco_build_reply(5000, "a", 0, out, need, NULL), 0);
    CHECK_INT(disco_build_reply(5000, "a", 0, out, 0, NULL), -1);
    CHECK_INT(disco_build_reply(5000, "a", 0, NULL, 64, NULL), -1);

    CASE(95, "ida e volta: o que enviamos, o nosso cliente le igual");
    CHECK_INT(disco_build_reply(6001, "sala \"A\" \\ Z\xc3\xa9", 7, out, sizeof out, &len), 0);
    CHECK_INT(disco_parse_reply(out, len, &srv), 0);
    CHECK_INT(srv.tcp_port, 6001);
    CHECK_STR(srv.name, "sala \"A\" \\ Z\xc3\xa9");
    CHECK_INT(srv.users, 7);
}

/* 96. O cliente: respostas de servidores (o nosso e os de outros grupos). */
static void caso_96_respostas(void)
{
    CASE(96, "a nossa, com e sem '\\n'");
    CHECK_INT(rep("{\"type\":\"SERVER\",\"tcp_port\":5000,\"name\":\"diogolap\",\"users\":3}\n"), 0);
    CHECK_INT(srv.tcp_port, 5000);
    CHECK_STR(srv.name, "diogolap");
    CHECK_INT(srv.users, 3);
    CHECK_STR(srv.ip, "");                   /* o IP e de quem chama */
    CHECK_INT(rep("{\"type\":\"SERVER\",\"tcp_port\":5000}"), 0);
    CHECK_INT(srv.tcp_port, 5000);
    CHECK_STR(srv.name, "");                 /* name e users opcionais */
    CHECK_INT(srv.users, -1);

    CASE(96, "o formato de outros grupos: \"port\", com ou sem type");
    CHECK_INT(rep("{\"port\":5000}"), 0);
    CHECK_INT(srv.tcp_port, 5000);
    CHECK_INT(rep("{\"type\":\"SERVER\",\"port\":6000,\"extra\":[1,2]}"), 0);
    CHECK_INT(srv.tcp_port, 6000);
    CHECK_INT(rep("{\"port\":1e3}"), 0);     /* um numero JSON inteiro e inteiro */
    CHECK_INT(srv.tcp_port, 1000);

    CASE(96, "\"tcp_port\" ganha a \"port\"; um tcp_port mal = resposta recusada");
    CHECK_INT(rep("{\"tcp_port\":5000,\"port\":6000}"), 0);
    CHECK_INT(srv.tcp_port, 5000);
    CHECK_INT(rep("{\"tcp_port\":\"x\",\"port\":6000}"), -1);

    CASE(96, "um IP na mensagem e ignorado (typical-problems 4.4)");
    CHECK_INT(rep("{\"type\":\"SERVER\",\"tcp_port\":5000,\"ip\":\"10.9.9.9\",\"host\":\"h\"}"), 0);
    CHECK_STR(srv.ip, "");

    CASE(96, "porta invalida ou em falta: -1");
    CHECK_INT(rep("{\"type\":\"SERVER\"}"), -1);
    CHECK_INT(rep("{\"tcp_port\":0}"), -1);
    CHECK_INT(rep("{\"tcp_port\":65536}"), -1);
    CHECK_INT(rep("{\"tcp_port\":-1}"), -1);
    CHECK_INT(rep("{\"tcp_port\":5000.5}"), -1);
    CHECK_INT(rep("{\"tcp_port\":\"5000\"}"), -1);
    CHECK_INT(rep("{\"tcp_port\":null}"), -1);
    CHECK_INT(rep("{\"tcp_port\":true}"), -1);
    CHECK_INT(rep("{\"tcp_port\":1e300}"), -1);

    CASE(96, "outro type: nao e uma resposta");
    CHECK_INT(rep("{\"type\":\"DISCOVER\",\"tcp_port\":5000}"), -1);
    CHECK_INT(rep("{\"type\":\"server\",\"tcp_port\":5000}"), -1);
    CHECK_INT(rep("{\"type\":5,\"tcp_port\":5000}"), -1);
    CHECK_INT(rep(DISCO_REQUEST), -1);

    CASE(96, "users mal: fica -1, mas a resposta vale");
    CHECK_INT(rep("{\"tcp_port\":5000,\"users\":\"3\"}"), 0);
    CHECK_INT(srv.users, -1);
    CHECK_INT(rep("{\"tcp_port\":5000,\"users\":-2}"), 0);
    CHECK_INT(srv.users, -1);
    CHECK_INT(rep("{\"tcp_port\":5000,\"users\":1.5}"), 0);
    CHECK_INT(srv.users, -1);
    CHECK_INT(rep("{\"tcp_port\":5000,\"users\":0}"), 0);
    CHECK_INT(srv.users, 0);

    CASE(96, "name: nao-string ignorado; controlo -> '?'");
    CHECK_INT(rep("{\"tcp_port\":5000,\"name\":42}"), 0);
    CHECK_STR(srv.name, "");
    CHECK_INT(rep("{\"tcp_port\":5000,\"name\":\"a\\u001b[2Jb\"}"), 0);
    CHECK_STR(srv.name, "a?[2Jb");
    CHECK_INT(rep("{\"tcp_port\":5000,\"name\":\"a\\nb\\u007f\"}"), 0);
    CHECK_STR(srv.name, "a?b?");

    CASE(96, "name comprido: cortado aos 64 bytes sem partir um caracter");
    {
        char name[200], json[300];
        memset(name, 'x', 100);
        name[100] = '\0';
        snprintf(json, sizeof json, "{\"tcp_port\":5000,\"name\":\"%s\"}", name);
        CHECK_INT(rep(json), 0);
        CHECK_INT(strlen(srv.name), DISCO_NAME_MAX);

        memset(name, 'a', 63);               /* 63 + "é" (2 bytes) = 65 */
        strcpy(name + 63, "\xc3\xa9");
        snprintf(json, sizeof json, "{\"tcp_port\":5000,\"name\":\"%s\"}", name);
        CHECK_INT(rep(json), 0);
        CHECK_INT(strlen(srv.name), 63);     /* o "é" sai inteiro, nao a meio */

        memset(name, 'a', 62);               /* 62 + "é" = 64: cabe inteiro */
        strcpy(name + 62, "\xc3\xa9");
        snprintf(json, sizeof json, "{\"tcp_port\":5000,\"name\":\"%s\"}", name);
        CHECK_INT(rep(json), 0);
        CHECK_STR(srv.name, name);
    }

    CASE(96, "lixo: -1, e *out a zeros (users -1), nunca meio preenchido");
    memset(&srv, 'z', sizeof srv);
    CHECK_INT(rep("SERVER 5000"), -1);
    CHECK_INT(srv.tcp_port, 0);
    CHECK_STR(srv.name, "");
    CHECK_STR(srv.ip, "");
    CHECK_INT(srv.users, -1);
    CHECK_INT(rep(""), -1);
    CHECK_INT(rep("{"), -1);
    CHECK_INT(rep("[5000]"), -1);
    CHECK_INT(rep("5000"), -1);
    CHECK_INT(rep("{\"port\":5000}{\"port\":6000}"), -1);
    CHECK_INT(disco_parse_reply("{\"port\":5000}\0", 14, &srv), -1);
    CHECK_INT(rep("{\"port\":5000,\"name\":\"\xff\"}"), -1);
    CHECK_INT(disco_parse_reply(NULL, 0, &srv), -1);

    CASE(96, "tamanho: 512 ainda e lido, 513 ja nao");
    {
        /* {"port":5000,"pad":"aaa..."} com exatamente 512 bytes */
        strcpy(big, "{\"port\":5000,\"pad\":\"");
        size_t k = strlen(big);
        memset(big + k, 'a', DISCO_DGRAM_MAX - k - 2);
        strcpy(big + DISCO_DGRAM_MAX - 2, "\"}");
    }
    CHECK_INT(strlen(big), DISCO_DGRAM_MAX);
    CHECK_INT(disco_parse_reply(big, DISCO_DGRAM_MAX, &srv), 0);
    CHECK_INT(srv.tcp_port, 5000);
    strcpy(big + DISCO_DGRAM_MAX - 2, "a\"}");       /* mais um 'a': 513 */
    CHECK_INT(strlen(big), DISCO_DGRAM_MAX + 1);
    CHECK_INT(disco_parse_reply(big, DISCO_DGRAM_MAX + 1, &srv), -1);
}

/* 97. O name que o servidor anuncia (-N ou o hostname). */
static void caso_97_nomes(void)
{
    char n64[DISCO_NAME_MAX + 2];
    memset(n64, 'n', DISCO_NAME_MAX + 1);
    n64[DISCO_NAME_MAX + 1] = '\0';

    CASE(97, "validos: ASCII, UTF-8, espacos e aspas, 64 bytes");
    CHECK_INT(disco_valid_name("diogolap"), 1);
    CHECK_INT(disco_valid_name("Z\xc3\xa9"), 1);
    CHECK_INT(disco_valid_name("sala do \"grupo\" 3"), 1);
    CHECK_INT(disco_valid_name(n64 + 1), 1);         /* 64 'n' */

    CASE(97, "invalidos: vazio, NULL, 65 bytes, controlo, UTF-8 partido");
    CHECK_INT(disco_valid_name(""), 0);
    CHECK_INT(disco_valid_name(NULL), 0);
    CHECK_INT(disco_valid_name(n64), 0);             /* 65 'n' */
    CHECK_INT(disco_valid_name("a\tb"), 0);
    CHECK_INT(disco_valid_name("a\x7f"), 0);
    CHECK_INT(disco_valid_name("\xff"), 0);
    CHECK_INT(disco_valid_name("Z\xc3"), 0);
}

/* 98. A lista do cliente: sem repetidos (3 envios x N interfaces). */
static void caso_98_lista(void)
{
    struct disco_list   l;
    struct disco_server a = mk("192.168.1.10", 5000, "diogolap", 3);

    CASE(98, "comeca vazia; um novo entra");
    disco_list_init(&l);
    CHECK_INT(l.n, 0);
    CHECK_INT(disco_list_add(&l, &a), 1);
    CHECK_INT(l.n, 1);

    CASE(98, "mesmo IP e porta: repetido, atualiza name e users, mesmo lugar");
    struct disco_server a2 = mk("192.168.1.10", 5000, "novo", 4);
    CHECK_INT(disco_list_add(&l, &a2), 0);
    CHECK_INT(disco_list_add(&l, &a2), 0);
    CHECK_INT(l.n, 1);
    CHECK_STR(l.s[0].name, "novo");
    CHECK_INT(l.s[0].users, 4);

    CASE(98, "outra porta no mesmo IP, ou outro IP: servidores diferentes");
    struct disco_server b = mk("192.168.1.10", 5002, "", -1);
    struct disco_server c = mk("192.168.1.11", 5000, "", -1);
    CHECK_INT(disco_list_add(&l, &c), 1);
    CHECK_INT(disco_list_add(&l, &b), 1);
    CHECK_INT(l.n, 3);

    CASE(98, "ordenada: com name primeiro; depois IP e porta");
    CHECK_STR(l.s[0].name, "novo");
    CHECK_INT(l.s[1].tcp_port, 5002);                /* .10 antes de .11, */
    CHECK_STR(l.s[2].ip, "192.168.1.11");            /* embora tenha chegado depois */

    CASE(98, "name sem contar maiusculas; IP como numero; a ordem de chegada nao conta");
    disco_list_init(&l);
    const char *ips[] = { "10.0.0.10", "10.0.0.9", "10.0.0.9" };
    const int   ports[] = { 5000, 6000, 5000 };
    struct disco_server z = mk("10.0.0.1", 5000, "zeta", -1);
    struct disco_server A = mk("10.0.0.2", 5000, "Alfa", -1);
    struct disco_server m = mk("10.0.0.3", 5000, "meio", -1);
    CHECK_INT(disco_list_add(&l, &z), 1);
    for (int i = 0; i < 3; i++) {
        struct disco_server u = mk(ips[i], ports[i], "", -1);
        CHECK_INT(disco_list_add(&l, &u), 1);
    }
    CHECK_INT(disco_list_add(&l, &m), 1);
    CHECK_INT(disco_list_add(&l, &A), 1);
    CHECK_STR(l.s[0].name, "Alfa");
    CHECK_STR(l.s[1].name, "meio");
    CHECK_STR(l.s[2].name, "zeta");
    CHECK_STR(l.s[3].ip, "10.0.0.9");                /* 9 < 10, como numero */
    CHECK_INT(l.s[3].tcp_port, 5000);
    CHECK_STR(l.s[4].ip, "10.0.0.9");
    CHECK_INT(l.s[4].tcp_port, 6000);
    CHECK_STR(l.s[5].ip, "10.0.0.10");

    CASE(98, "um repetido que muda de name muda de lugar");
    struct disco_server z2 = mk("10.0.0.1", 5000, "aaa", -1);
    CHECK_INT(disco_list_add(&l, &z2), 0);
    CHECK_STR(l.s[0].name, "aaa");
    CHECK_STR(l.s[1].name, "Alfa");
    CHECK_INT(l.n, 6);

    CASE(98, "cheia: -1 e nao entra; um repetido continua a ser reconhecido");
    disco_list_init(&l);
    for (int i = 0; i < DISCO_SERVERS_MAX; i++) {
        struct disco_server s = mk("10.0.0.1", 6000 + i, "", -1);
        CHECK_INT(disco_list_add(&l, &s), 1);
    }
    struct disco_server extra = mk("10.0.0.2", 5000, "", -1);
    CHECK_INT(disco_list_add(&l, &extra), -1);
    CHECK_INT(l.n, DISCO_SERVERS_MAX);
    struct disco_server again = mk("10.0.0.1", 6000, "x", 1);
    CHECK_INT(disco_list_add(&l, &again), 0);
}

/* 99. A linha que o cliente mostra para cada servidor encontrado. */
static void caso_99_formato(void)
{
    char                line[128];
    struct disco_server s = mk("192.168.1.10", 5000, "diogolap", 3);

    CASE(99, "completa");
    CHECK_INT(disco_format(&s, 1, line, sizeof line), 0);
    CHECK_STR(line, "1) 192.168.1.10:5000  diogolap  (3 online)");

    CASE(99, "sem name, sem users, e as combinacoes");
    s = mk("10.0.0.7", 5000, "", -1);
    CHECK_INT(disco_format(&s, 2, line, sizeof line), 0);
    CHECK_STR(line, "2) 10.0.0.7:5000");
    s = mk("10.0.0.7", 5000, "", 0);
    CHECK_INT(disco_format(&s, 3, line, sizeof line), 0);
    CHECK_STR(line, "3) 10.0.0.7:5000  (0 online)");
    s = mk("10.0.0.7", 5000, "sala", -1);
    CHECK_INT(disco_format(&s, 10, line, sizeof line), 0);
    CHECK_STR(line, "10) 10.0.0.7:5000  sala");

    CASE(99, "IPv6 entre parenteses retos");
    s = mk("fe80::1", 5000, "sala", -1);
    CHECK_INT(disco_format(&s, 1, line, sizeof line), 0);
    CHECK_STR(line, "1) [fe80::1]:5000  sala");

    CASE(99, "nao cabe: -1 e string vazia; a medida certa cabe");
    s = mk("192.168.1.10", 5000, "diogolap", 3);
    size_t need = strlen("1) 192.168.1.10:5000  diogolap  (3 online)") + 1;
    CHECK_INT(disco_format(&s, 1, line, need - 1), -1);
    CHECK_STR(line, "");
    CHECK_INT(disco_format(&s, 1, line, need), 0);
    CHECK_INT(disco_format(&s, 1, NULL, 10), -1);
    CHECK_INT(disco_format(&s, 1, line, 0), -1);
}

int main(void)
{
    caso_94_pedidos();
    caso_95_resposta();
    caso_96_respostas();
    caso_97_nomes();
    caso_98_lista();
    caso_99_formato();
    return t_report("disco");
}
