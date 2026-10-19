/*
 * disco.c — M5: os datagramas da descoberta (pedido, resposta, lista).
 * Contrato e porquês em include/disco.h.
 *
 * As mesmas duas regras de memória do protocol.c:
 *   1. cada cJSON_Parse*() / cJSON_CreateObject() tem UM cJSON_Delete(), em
 *      TODOS os caminhos — daí a saída única (`out:`) em cada função;
 *   2. o que cJSON_PrintUnformatted() devolve é libertado com cJSON_free().
 * O `make test SAN=1` (LeakSanitizer) apanha qualquer falha.
 */
#include "disco.h"
#include "protocol.h"          /* proto_utf8_valid(): a mesma regra das linhas */

#include <cjson/cJSON.h>
#include <limits.h>
#include <netinet/in.h>        /* INET6_ADDRSTRLEN */
#include <stdio.h>
#include <string.h>

_Static_assert(DISCO_IP_BUF == INET6_ADDRSTRLEN,
               "DISCO_IP_BUF tem de ser o INET6_ADDRSTRLEN");

#define REQ_TYPE   "DISCOVER"
#define REPLY_TYPE "SERVER"

static int is_control(unsigned char c)
{
    return c < 0x20 || c == 0x7F;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* O que qualquer datagrama que lemos tem de cumprir ANTES do JSON:
 *   - tamanho: 1..DISCO_DGRAM_MAX (disco.h: maior do que isso chega cortado);
 *   - sem '\0' embebido: o cJSON pararia nele e leria só metade;
 *   - UTF-8 válido (RFC 8259 §8.1): um name com bytes inválidos rebentava um
 *     cliente em Python ao fazer json.loads — o P38 do M4, agora em UDP. */
static int dgram_ok(const char *buf, size_t len)
{
    return buf != NULL && len > 0 && len <= DISCO_DGRAM_MAX &&
           memchr(buf, '\0', len) == NULL && proto_utf8_valid(buf, len);
}

/* O datagrama inteiro é UM objeto JSON, só com espaços à volta? Devolve a
 * árvore (quem recebe != NULL faz o cJSON_Delete) ou NULL.
 * "{...}{...}" é recusado, como nas linhas do TCP: aceitar o primeiro e
 * ignorar o resto é uma falha silenciosa. */
static cJSON *parse_object(const char *buf, size_t len)
{
    const char *end  = NULL;
    cJSON      *root = cJSON_ParseWithLengthOpts(buf, len, &end, 0);
    if (root == NULL)
        return NULL;
    for (const char *p = end; p < buf + len; p++) {
        if (!is_space(*p)) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    if (!cJSON_IsObject(root)) {            /* ["DISCOVER"], "DISCOVER", 42 */
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* "type" existe e é exatamente esta string? (os nomes contam maiúsculas, S3) */
static int type_is(const cJSON *root, const char *want)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "type");
    return cJSON_IsString(t) && t->valuestring != NULL &&
           strcmp(t->valuestring, want) == 0;
}

/* Um número JSON INTEIRO em [lo, hi]? 1 e o valor em *v; senão 0.
 * O cJSON guarda os números como double, e 5000.5 tem valueint 5000 — por
 * isso compara-se o double (como o "code" do ERROR no protocol.c). A ordem
 * importa: primeiro o intervalo, e só depois o (long)d, que fora do intervalo
 * de um long seria comportamento indefinido. Um NaN falha o intervalo. */
static int get_int(const cJSON *obj, const char *key, long lo, long hi, long *v)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it))
        return 0;
    double d = it->valuedouble;
    if (!(d >= (double)lo && d <= (double)hi) || d != (double)(long)d)
        return 0;
    *v = (long)d;
    return 1;
}

/* Copia um name RECEBIDO para se poder mostrar:
 *   - até DISCO_NAME_MAX bytes, sem partir um caracter UTF-8 a meio: o texto
 *     já é UTF-8 válido (dgram_ok, e o cJSON gera UTF-8 válido dos \uXXXX),
 *     por isso basta não cortar ANTES de um byte de continuação (10xxxxxx);
 *   - cada caracter de controlo passa a '?' (um ESC limparia o ecrã, C10). */
static void clean_name(const char *src, char dst[DISCO_NAME_MAX + 1])
{
    size_t n = strlen(src);
    if (n > DISCO_NAME_MAX) {
        n = DISCO_NAME_MAX;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = is_control(c) ? '?' : (char)c;
    }
    dst[n] = '\0';
}

/* ---- Servidor ----------------------------------------------------------- */

int disco_is_request(const char *buf, size_t len)
{
    if (!dgram_ok(buf, len))
        return 0;

    /* O texto simples "DISCOVER", com espaços, '\r' e '\n' à volta. */
    size_t a = 0, b = len;
    while (a < b && is_space(buf[a]))
        a++;
    while (b > a && is_space(buf[b - 1]))
        b--;
    if (b - a == strlen(REQ_TYPE) && memcmp(buf + a, REQ_TYPE, b - a) == 0)
        return 1;

    /* Ou um objeto JSON com "type":"DISCOVER" (campos a mais são ignorados). */
    cJSON *root = parse_object(buf, len);
    if (root == NULL)
        return 0;
    int ok = type_is(root, REQ_TYPE);
    cJSON_Delete(root);
    return ok;
}

int disco_valid_name(const char *name)
{
    if (name == NULL)
        return 0;
    size_t n = strnlen(name, DISCO_NAME_MAX + 1);   /* nunca lê além de max+1 */
    if (n < 1 || n > DISCO_NAME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (is_control((unsigned char)name[i]))
            return 0;
    return proto_utf8_valid(name, n);
}

int disco_build_reply(int tcp_port, const char *name, int users,
                      char *out, size_t cap, size_t *out_len)
{
    if (out == NULL || cap == 0)
        return -1;
    out[0] = '\0';                          /* nunca meio preenchido */
    if (tcp_port < 1 || tcp_port > 65535 || users < 0 || !disco_valid_name(name))
        return -1;

    int    rc   = -1;
    char  *json = NULL;
    cJSON *root = cJSON_CreateObject();     /* daqui para baixo: saída única */
    if (root == NULL)
        goto out;

    /* A ordem de inserção é a ordem no fio: igual ao exemplo do disco.h. */
    if (cJSON_AddStringToObject(root, "type", REPLY_TYPE) == NULL ||
        cJSON_AddNumberToObject(root, "tcp_port", tcp_port) == NULL ||
        cJSON_AddStringToObject(root, "name", name) == NULL ||
        cJSON_AddNumberToObject(root, "users", users) == NULL)
        goto out;

    json = cJSON_PrintUnformatted(root);    /* nunca cJSON_Print: mete '\n' */
    if (json == NULL)
        goto out;

    size_t n = strlen(json);
    /* + '\n' + '\0'. O _Static_assert do disco.h diz que o datagrama cabe
     * sempre em DISCO_DGRAM_MAX; verifica-se na mesma. */
    if (n + 2 > cap || n + 1 > DISCO_DGRAM_MAX)
        goto out;
    memcpy(out, json, n);
    out[n]     = '\n';
    out[n + 1] = '\0';
    if (out_len != NULL)
        *out_len = n + 1;
    rc = 0;

out:
    cJSON_free(json);                       /* cJSON_free(NULL) é seguro */
    cJSON_Delete(root);
    return rc;
}

/* ---- Cliente ------------------------------------------------------------ */

static void server_reset(struct disco_server *s)
{
    memset(s, 0, sizeof *s);
    s->users = -1;
}

int disco_parse_reply(const char *buf, size_t len, struct disco_server *out)
{
    server_reset(out);
    if (!dgram_ok(buf, len))
        return -1;
    cJSON *root = parse_object(buf, len);
    if (root == NULL)
        return -1;

    int rc = -1;                            /* daqui para baixo: saída única */

    /* "type" ausente (o {"port":N} de outros grupos) ou "SERVER". Outro tipo
     * qualquer — um DISCOVER de outro cliente, um OK — não é uma resposta. */
    if (cJSON_GetObjectItemCaseSensitive(root, "type") != NULL &&
        !type_is(root, REPLY_TYPE))
        goto out;

    /* A porta: "tcp_port" (o nosso e o do grupo 1) se vier, senão "port".
     * Se "tcp_port" vier MAL não se tenta o "port": a mensagem contradiz-se. */
    long        port;
    const char *key = cJSON_GetObjectItemCaseSensitive(root, "tcp_port") != NULL
                          ? "tcp_port" : "port";
    if (!get_int(root, key, 1, 65535, &port))
        goto out;

    /* Opcionais: se vierem mal, fica-se sem eles e a resposta vale na mesma. */
    struct disco_server s;
    server_reset(&s);
    s.tcp_port = (int)port;

    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(nm) && nm->valuestring != NULL)
        clean_name(nm->valuestring, s.name);

    long users;
    if (get_int(root, "users", 0, INT_MAX, &users))
        s.users = (int)users;

    *out = s;                               /* só no fim: nunca meio preenchido */
    rc   = 0;

out:
    cJSON_Delete(root);
    return rc;
}

void disco_list_init(struct disco_list *l)
{
    memset(l, 0, sizeof *l);
}

int disco_list_add(struct disco_list *l, const struct disco_server *srv)
{
    for (size_t i = 0; i < l->n; i++) {
        struct disco_server *s = &l->s[i];
        if (s->tcp_port == srv->tcp_port && strcmp(s->ip, srv->ip) == 0) {
            memcpy(s->name, srv->name, sizeof s->name);
            s->users = srv->users;
            return 0;
        }
    }
    if (l->n >= DISCO_SERVERS_MAX)
        return -1;
    l->s[l->n++] = *srv;
    return 1;
}

int disco_format(const struct disco_server *srv, size_t idx, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return -1;

    char users[32] = "";
    if (srv->users >= 0)
        snprintf(users, sizeof users, "  (%d online)", srv->users);

    int v6 = strchr(srv->ip, ':') != NULL;  /* "[fe80::1]:5000", como num URL */
    int k  = snprintf(out, cap, "%zu) %s%s%s:%d%s%s%s", idx,
                      v6 ? "[" : "", srv->ip, v6 ? "]" : "", srv->tcp_port,
                      srv->name[0] != '\0' ? "  " : "", srv->name, users);
    if (k < 0 || (size_t)k >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}
