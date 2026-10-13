/*
 * protocol.c — linhas <-> mensagens validadas, com cJSON.
 * Contrato em include/protocol.h; o porquê em docs/protocol-layer.md.
 *
 * Duas regras de memória que este ficheiro nunca pode quebrar:
 *   1. cada cJSON_Parse*() / cJSON_CreateObject() tem UM cJSON_Delete(), em
 *      TODOS os caminhos — por isso cada função tem uma única saída (`out:`);
 *   2. o que cJSON_PrintUnformatted() devolve é libertado com cJSON_free().
 * O `make test SAN=1` (LeakSanitizer) apanha qualquer falha a qualquer uma.
 */
#include "protocol.h"

#include <cjson/cJSON.h>
#include <string.h>

/* ---- Regras de conteúdo (partilhadas por parse e encode) ---------------- */

static int is_control(unsigned char c)
{
    return c < 0x20 || c == 0x7F;
}

/* min..max bytes, sem caracteres de controlo e, se no_space, sem ' '. */
static int valid_str(const char *s, size_t min, size_t max, int no_space)
{
    size_t n = strnlen(s, max + 1);         /* nunca lê mais do que max+1 */
    if (n < min || n > max)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_control(c) || (no_space && c == ' '))
            return 0;
    }
    return 1;
}

int proto_valid_nick(const char *s)    { return valid_str(s, 1, PROTO_NICK_MAX, 1); }
int proto_valid_text(const char *s)    { return valid_str(s, 1, PROTO_TEXT_MAX, 0); }
static int valid_reason(const char *s) { return valid_str(s, 0, PROTO_REASON_MAX, 0); }
static int valid_code(int code)        { return code >= 100 && code <= 999; }

/* ---- Tabela de tipos ---------------------------------------------------- */

static const struct { enum msg_type t; const char *name; } TYPES[] = {
    { MT_NICK,  "NICK"  },
    { MT_MSG,   "MSG"   },
    { MT_OK,    "OK"    },
    { MT_CHAT,  "CHAT"  },
    { MT_ERROR, "ERROR" },
};
#define N_TYPES (sizeof TYPES / sizeof TYPES[0])

const char *proto_type_name(enum msg_type t)
{
    for (size_t i = 0; i < N_TYPES; i++)
        if (TYPES[i].t == t)
            return TYPES[i].name;
    return "?";
}

/* strcmp, não strcasecmp: "nick" não é "NICK" (teste #15). 0 = desconhecido. */
static enum msg_type type_from_name(const char *name)
{
    for (size_t i = 0; i < N_TYPES; i++)
        if (strcmp(TYPES[i].name, name) == 0)
            return TYPES[i].t;
    return 0;
}

/* ---- Parse -------------------------------------------------------------- */

/* Lê um campo string obrigatório para dst. As três verificações, por esta
 * ordem: existe, é string, respeita as regras. Só depois se toca em
 * valuestring — que é NULL num campo ausente ou não-string. */
static int get_str(const cJSON *obj, const char *key, char *dst, size_t dst_cap,
                   int (*valid)(const char *))
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it) || it->valuestring == NULL)
        return 0;
    if (!valid(it->valuestring))
        return 0;
    /* valid() garantiu strlen <= max == dst_cap - 1: cabe com o '\0'. */
    strncpy(dst, it->valuestring, dst_cap - 1);
    dst[dst_cap - 1] = '\0';
    return 1;
}

static int is_json_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int proto_parse(const char *line, size_t len, struct msg *out, struct proto_err *err)
{
    memset(out, 0, sizeof *out);            /* nunca meio preenchido */
    err->code   = 400;                      /* nesta camada é sempre 400 */
    err->reason = PROTO_R_JSON;

    /* '\0' embebido: o cJSON pararia nele e leria só metade (teste #29). */
    if (memchr(line, '\0', len) != NULL)
        return -1;

    /* ParseWithLength(Opts) e não cJSON_Parse: a linha NÃO é terminada em
     * '\0' garantidamente, e queremos saber onde o parse acabou. */
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(line, len, &end, 0);
    if (root == NULL)
        return -1;                          /* nada alocado: sair já é seguro */

    int rc = -1;                            /* daqui para baixo: saída única */

    /* Só espaços depois do objeto. "{...}{...}" -> 400 (teste #25). */
    for (const char *p = end; p < line + len; p++)
        if (!is_json_space(*p))
            goto out;

    if (!cJSON_IsObject(root))              /* [1,2], "x", null, 42 (#28) */
        goto out;

    err->reason = PROTO_R_FIELD;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL)
        goto out;

    out->type = type_from_name(type->valuestring);
    switch (out->type) {
    case MT_NICK:
        if (!get_str(root, "nick", out->nick, sizeof out->nick, proto_valid_nick))
            goto out;
        break;

    case MT_MSG:
        if (!get_str(root, "text", out->text, sizeof out->text, proto_valid_text))
            goto out;
        break;

    case MT_OK:
        break;

    case MT_CHAT: {
        if (!get_str(root, "from", out->from, sizeof out->from, proto_valid_nick) ||
            !get_str(root, "text", out->text, sizeof out->text, proto_valid_text))
            goto out;
        /* "private" é opcional: ausente = false; presente tem de ser bool. */
        const cJSON *pv = cJSON_GetObjectItemCaseSensitive(root, "private");
        if (pv != NULL && !cJSON_IsBool(pv))
            goto out;
        out->is_private = cJSON_IsTrue(pv);
        break;
    }

    case MT_ERROR: {
        /* O cJSON guarda números como double. 409.5 tem valueint 409, por
         * isso compara-se o double com o inteiro para recusar frações. */
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (!cJSON_IsNumber(c) || c->valuedouble != (double)c->valueint ||
            !valid_code(c->valueint))
            goto out;
        out->code = c->valueint;
        /* "reason" é opcional: ausente = "". */
        if (cJSON_GetObjectItemCaseSensitive(root, "reason") != NULL &&
            !get_str(root, "reason", out->reason, sizeof out->reason, valid_reason))
            goto out;
        break;
    }

    default:                                /* string válida, tipo desconhecido */
        err->reason = PROTO_R_TYPE;
        goto out;
    }

    rc = 0;

out:
    cJSON_Delete(root);                     /* o ÚNICO Delete, para todos os caminhos */
    if (rc != 0)
        memset(out, 0, sizeof *out);        /* falhou a meio: não deixar restos */
    return rc;
}

/* ---- Encode ------------------------------------------------------------- */

/* Valida pelas MESMAS regras do parse: nunca enviamos o que recusaríamos. */
static int valid_msg(const struct msg *m)
{
    switch (m->type) {
    case MT_NICK:  return proto_valid_nick(m->nick);
    case MT_MSG:   return proto_valid_text(m->text);
    case MT_OK:    return 1;
    case MT_CHAT:  return proto_valid_nick(m->from) && proto_valid_text(m->text);
    case MT_ERROR: return valid_code(m->code) && valid_reason(m->reason);
    default:       return 0;
    }
}

int proto_encode(const struct msg *m, char *out, size_t cap, size_t *out_len)
{
    if (m == NULL || out == NULL || cap < PROTO_LINE_BUF || !valid_msg(m))
        return -1;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return -1;

    int   rc   = -1;
    char *json = NULL;

    /* "type" primeiro: a ordem não importa ao parse, mas é assim no slide e
     * é o que torna os bytes iguais aos exemplos (teste #31). Cada Add*
     * devolve NULL se a alocação falhar. */
    if (!cJSON_AddStringToObject(root, "type", proto_type_name(m->type)))
        goto out;

    switch (m->type) {
    case MT_NICK:
        if (!cJSON_AddStringToObject(root, "nick", m->nick)) goto out;
        break;
    case MT_MSG:
        if (!cJSON_AddStringToObject(root, "text", m->text)) goto out;
        break;
    case MT_OK:
        break;
    case MT_CHAT:
        if (!cJSON_AddStringToObject(root, "from", m->from) ||
            !cJSON_AddStringToObject(root, "text", m->text) ||
            !cJSON_AddBoolToObject(root, "private", m->is_private != 0))
            goto out;
        break;
    case MT_ERROR:
        if (!cJSON_AddNumberToObject(root, "code", m->code) ||
            !cJSON_AddStringToObject(root, "reason", m->reason))
            goto out;
        break;
    }

    /* Unformatted, NUNCA cJSON_Print(): esse mete '\n' e indentação, e um
     * '\n' a meio parte a mensagem em duas no framing do outro lado. */
    json = cJSON_PrintUnformatted(root);
    if (json == NULL)
        goto out;

    size_t n = strlen(json);
    if (n + 1 > RX_MAX_LINE)                /* + '\n'. O _Static_assert diz que */
        goto out;                           /* nunca acontece; verifica-se na mesma */

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
