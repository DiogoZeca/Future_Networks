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

/* min..max bytes, sem caracteres de controlo nem nenhum dos bytes de `banned`
 * (NULL = nenhum). */
static int valid_str(const char *s, size_t min, size_t max, const char *banned)
{
    size_t n = strnlen(s, max + 1);         /* nunca lê mais do que max+1 */
    if (n < min || n > max)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_control(c) || (banned != NULL && strchr(banned, c) != NULL))
            return 0;
    }
    return 1;
}

/* Nick estrito (D4 + D20): o que aceitamos num NICK/PRIV e o que enviamos.
 * Sem ' ' (o /msg e o USERS partem por espaço) e sem '"' nem '\\' (o JSON
 * escapava-os para 2 bytes e o pior USERS deixava de caber numa linha). */
int proto_valid_nick(const char *s)    { return valid_str(s, 1, PROTO_NICK_MAX, " \"\\"); }
/* text: 1..4096 bytes, e desde a revisão do M3 PODE ter caracteres de
 * controlo (mensagens de várias linhas). Só o tamanho conta. */
int proto_valid_text(const char *s)
{
    size_t n = strnlen(s, PROTO_TEXT_MAX + 1);
    return n >= 1 && n <= PROTO_TEXT_MAX;
}
static int valid_reason(const char *s) { return valid_str(s, 0, PROTO_REASON_MAX, NULL); }
static int valid_code(int code)        { return code >= 100 && code <= 999; }

/* Nick vindo de um SERVIDOR (CHAT.from/to, USERS): regra larga, só o que é
 * preciso para o mostrar e para o partir por espaços. Tolerante a receber:
 * um servidor de outro grupo que aceite aspas nos nicks não nos parte. */
static int valid_peer_nick(const char *s) { return valid_str(s, 1, PROTO_PEER_NICK_MAX, " "); }

/* ---- Comparação de nicks (D18) ------------------------------------------ */

/* tolower() só para ASCII. Não se usa o tolower() da libc: depende do locale
 * e, com um locale latino, podia dobrar bytes >= 0x80 de forma diferente
 * consoante a máquina — e dois servidores discordavam do que é "o mesmo". */
static unsigned char fold(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

int proto_nick_cmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i = 0;
    for (; x[i] != '\0' && fold(x[i]) == fold(y[i]); i++)
        ;
    if (fold(x[i]) != fold(y[i]))
        return fold(x[i]) < fold(y[i]) ? -1 : 1;
    return strcmp(a, b);                    /* empate: "Ana" antes de "ana" */
}

int proto_nick_eq(const char *a, const char *b)
{
    for (; *a != '\0' && fold((unsigned char)*a) == fold((unsigned char)*b); a++, b++)
        ;
    return *a == '\0' && *b == '\0';
}

/* ---- USERS: nicks numa só string ---------------------------------------- */

int proto_users_add(struct msg *m, const char *nick)
{
    if (m->type != MT_USERS || !proto_valid_nick(nick) || m->n_nicks >= PROTO_USERS_MAX)
        return -1;
    size_t used = strlen(m->nicks);
    size_t need = (used ? 1 : 0) + strlen(nick) + 1;   /* espaço + nick + '\0' */
    if (used + need > sizeof m->nicks)                 /* nunca com <= 64 nicks */
        return -1;
    if (used)
        m->nicks[used++] = ' ';
    strcpy(m->nicks + used, nick);
    m->n_nicks++;
    return 0;
}

/* Valida a string empacotada de um USERS a ENVIAR: n_nicks nicks estritos,
 * separados por UM espaço, sem espaços nas pontas, no máximo PROTO_USERS_MAX.
 * Lista vazia ("" e 0) é válida. */
static int valid_users(const struct msg *m)
{
    if (m->n_nicks > PROTO_USERS_MAX || memchr(m->nicks, '\0', sizeof m->nicks) == NULL)
        return 0;
    size_t count = 0;
    const char *p = m->nicks;
    while (*p != '\0') {
        size_t n = strcspn(p, " ");
        char one[PROTO_NICK_MAX + 1];
        if (n == 0 || n > PROTO_NICK_MAX)   /* dois espaços seguidos, ou à cabeça */
            return 0;
        memcpy(one, p, n);
        one[n] = '\0';
        if (!proto_valid_nick(one))
            return 0;
        count++;
        p += n;
        if (*p == ' ' && *++p == '\0')     /* espaço no fim */
            return 0;
    }
    return count == m->n_nicks;
}

/* ---- Tabela de tipos ---------------------------------------------------- */

static const struct { enum msg_type t; const char *name; } TYPES[] = {
    { MT_NICK,  "NICK"  },
    { MT_MSG,   "MSG"   },
    { MT_OK,    "OK"    },
    { MT_CHAT,  "CHAT"  },
    { MT_ERROR, "ERROR" },
    { MT_EVENT, "EVENT" },
    { MT_PRIV,  "PRIV"  },
    { MT_LIST,  "LIST"  },
    { MT_QUIT,  "QUIT"  },
    { MT_USERS, "USERS" },
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

/* Um pedido com text (MSG, PRIV): 0 se é válido; senão a razão do ERROR 400.
 * "text too long" e não "invalid field" para o text grande de mais: é o único
 * caso que um utilizador provoca sem querer, e merece uma mensagem própria
 * (acordado com o primeiro grupo). */
static const char *get_request_text(const cJSON *obj, char *dst, size_t dst_cap)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, "text");
    if (!cJSON_IsString(it) || it->valuestring == NULL)
        return PROTO_R_FIELD;
    size_t n = strlen(it->valuestring);
    if (n == 0)
        return PROTO_R_FIELD;               /* text vazio continua proibido */
    if (n > PROTO_TEXT_MAX)
        return PROTO_R_TEXT_LONG;
    memcpy(dst, it->valuestring, n + 1);    /* n <= 4096 < dst_cap */
    (void)dst_cap;
    return NULL;
}

/* Um campo de texto livre vindo de um SERVIDOR (CHAT.text, EVENT.text,
 * ERROR.reason): tolerante. Tem de ser string; qualquer tamanho, vazio
 * incluído. Se não couber, é cortado sem partir um caracter UTF-8 a meio
 * (recua enquanto o primeiro byte que fica de fora for de continuação,
 * 10xxxxxx) e *truncated fica a 1. 0 = ok, -1 = não é string. */
static int get_peer_text(const cJSON *obj, const char *key, char *dst, size_t dst_cap,
                         int *truncated)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it) || it->valuestring == NULL)
        return -1;
    const char *v = it->valuestring;
    size_t n = strlen(v);
    if (n > dst_cap - 1) {
        n = dst_cap - 1;
        while (n > 0 && ((unsigned char)v[n] & 0xC0) == 0x80)
            n--;
        *truncated = 1;
    }
    memcpy(dst, v, n);
    dst[n] = '\0';
    return 0;
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

    case MT_MSG: {
        const char *why = get_request_text(root, out->text, sizeof out->text);
        if (why != NULL) {
            err->reason = why;
            goto out;
        }
        break;
    }

    case MT_OK:
        break;

    case MT_CHAT: {
        /* Vem de um servidor: regras largas (tolerante a receber). O text
         * pode vir vazio ou maior do que o nosso limite — mostra-se, cortado
         * se for preciso, em vez de se recusar a mensagem inteira. */
        if (!get_str(root, "from", out->from, sizeof out->from, valid_peer_nick) ||
            get_peer_text(root, "text", out->text, sizeof out->text, &out->truncated) != 0)
            goto out;
        /* "private" é opcional: ausente = false; presente tem de ser bool. */
        const cJSON *pv = cJSON_GetObjectItemCaseSensitive(root, "private");
        if (pv != NULL && !cJSON_IsBool(pv))
            goto out;
        out->is_private = cJSON_IsTrue(pv);
        /* Desde a revisão do M3 não há "to" no CHAT: se um servidor o mandar,
         * é um campo desconhecido e é ignorado, como qualquer outro. */
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
        /* "reason" é opcional: ausente = "". Texto livre de um servidor:
         * tolerante, como o text de um CHAT. */
        if (cJSON_GetObjectItemCaseSensitive(root, "reason") != NULL &&
            get_peer_text(root, "reason", out->reason, sizeof out->reason,
                          &out->truncated) != 0)
            goto out;
        break;
    }

    case MT_EVENT:
        /* Só "text". O slide 124 dá {"type":"EVENT","text":"rui joined"}:
         * texto livre de um servidor, tolerante como o de um CHAT. */
        if (get_peer_text(root, "text", out->text, sizeof out->text, &out->truncated) != 0)
            goto out;
        break;

    case MT_PRIV:
        /* {"type":"PRIV","to":"rui","text":"hi"} — slide 124. O "to" é um
         * nick ESTRITO: é um pedido de um cliente, e o destino tem de poder
         * existir no nosso servidor. */
        if (!get_str(root, "to", out->to, sizeof out->to, proto_valid_nick))
            goto out;
        {
            const char *why = get_request_text(root, out->text, sizeof out->text);
            if (why != NULL) {
                err->reason = why;
                goto out;
            }
        }
        break;

    case MT_LIST:                           /* sem campos */
    case MT_QUIT:
        break;

    case MT_USERS: {
        /* {"type":"USERS","nicks":["ana","rui"]} — o primeiro array do
         * protocolo. Cada elemento tem de ser string e nick (regra larga: vem
         * de um servidor). Lista vazia é aceite. Não há limite de contagem:
         * guarda-se o que couber em PROTO_NICKS_BUF e marca-se `truncated`
         * (um servidor federado pode ter centenas de pessoas). */
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "nicks");
        if (!cJSON_IsArray(arr))
            goto out;
        size_t used = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsString(it) || it->valuestring == NULL ||
                !valid_peer_nick(it->valuestring))
                goto out;
            size_t n = strlen(it->valuestring);
            if (used + (used ? 1 : 0) + n + 1 > sizeof out->nicks) {
                out->truncated = 1;         /* não cabe: os seguintes ficam de fora */
                continue;                   /* mas valida-os na mesma */
            }
            if (used)
                out->nicks[used++] = ' ';
            memcpy(out->nicks + used, it->valuestring, n + 1);
            used += n;
            out->n_nicks++;
        }
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
    case MT_EVENT: return proto_valid_text(m->text);
    case MT_PRIV:  return proto_valid_nick(m->to) && proto_valid_text(m->text);
    case MT_LIST:
    case MT_QUIT:  return 1;
    case MT_USERS: return valid_users(m);
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
        /* Exatamente os campos do slide. O m->to de um CHAT privado serve só
         * para o encaminhar (SESS_TO) e NÃO vai para o fio (revisão do M3). */
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
    case MT_EVENT:
        if (!cJSON_AddStringToObject(root, "text", m->text)) goto out;
        break;
    case MT_PRIV:
        if (!cJSON_AddStringToObject(root, "to", m->to) ||
            !cJSON_AddStringToObject(root, "text", m->text))
            goto out;
        break;
    case MT_LIST:
    case MT_QUIT:
        break;
    case MT_USERS: {
        /* O array é criado, preenchido e só DEPOIS pendurado no root: se
         * falhar a meio é preciso apagá-lo à mão, porque ainda não é do root. */
        cJSON *arr = cJSON_CreateArray();
        if (arr == NULL)
            goto out;
        const char *p = m->nicks;
        while (*p != '\0') {
            char   one[PROTO_NICK_MAX + 1];
            size_t n = strcspn(p, " ");     /* valid_users() garantiu n <= 32 */
            memcpy(one, p, n);
            one[n] = '\0';
            cJSON *str = cJSON_CreateString(one);
            if (str == NULL || !cJSON_AddItemToArray(arr, str)) {
                cJSON_Delete(str);
                cJSON_Delete(arr);
                goto out;
            }
            p += n;
            if (*p == ' ')
                p++;
        }
        if (!cJSON_AddItemToObject(root, "nicks", arr)) {
            cJSON_Delete(arr);
            goto out;
        }
        break;
    }
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
