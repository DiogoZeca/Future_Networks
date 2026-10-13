/*
 * protocol.h — linhas <-> mensagens validadas.
 *
 * Recebe o que o framing entrega (uma linha, já sem o '\n') e devolve uma
 * `struct msg` em que TODOS os campos foram verificados: existem, são do tipo
 * certo e respeitam os limites. O resto do programa nunca toca em cJSON — se
 * tem uma struct msg na mão, pode confiar nela.
 *
 * Não toca em sockets: é isso que a torna testável sem rede, como o framing.
 *
 *   socket --bytes--> framing --linhas--> protocol --struct msg--> lógica
 *
 * Tipos do M1 (slide 123: NICK e MSG, mais as respostas OK, CHAT e ERROR) e,
 * desde o M2, EVENT — o slide 123 põe "join and leave notices" no M2, não no
 * M3. PRIV, LIST, QUIT e USERS entram no M3; até lá são "unknown type".
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#include "framing.h"   /* RX_MAX_LINE */

/* ---- Limites (decisão D4 — proposta, TO AGREE com os outros grupos) ------ */

#define PROTO_NICK_MAX   32     /* bytes, não caracteres                     */
#define PROTO_TEXT_MAX   1024   /* bytes                                     */
#define PROTO_REASON_MAX 128    /* bytes, o "reason" de um ERROR             */

/* Regras de conteúdo (validadas no parse E no encode — nunca enviamos o que
 * recusaríamos):
 *   - nick:   1..32 bytes, sem espaços nem caracteres de controlo
 *             (sem espaços porque no M3 o `/msg <nick> texto` parte por espaço)
 *   - text:   1..1024 bytes, sem caracteres de controlo (logo sem '\n':
 *             mensagens de várias linhas não são suportadas)
 *   - reason: 0..128 bytes, sem caracteres de controlo
 * "Caracter de controlo" = byte < 0x20 ou 0x7F. UTF-8 não é validado (D3). */

/* Porque é que recusar caracteres de controlo também garante o enquadramento:
 * o cJSON escapa '"' e '\\' com 2 bytes e os caracteres de controlo com até 6
 * ("\u0001"). Sem estes últimos, nenhum byte cresce mais de 2x ao codificar, e
 * o pior caso tem um tamanho conhecido. O maior é um CHAT com from e text no
 * limite e cheios de '"':
 *     {"type":"CHAT","from":"","text":"","private":false}\n  = 52 bytes fixos
 *     + 2*32 + 2*1024                                         = 2164 bytes
 * O _Static_assert garante que isso cabe numa linha do framing — se alguém
 * subir os limites sem pensar, não compila. O teste #27 executa este pior caso. */
#define PROTO_JSON_OVERHEAD 64
_Static_assert(2 * (PROTO_NICK_MAX + PROTO_TEXT_MAX) + PROTO_JSON_OVERHEAD
                   <= RX_MAX_LINE,
               "o pior CHAT codificado tem de caber numa linha do framing");

/* Tamanho do out[] de proto_encode(): linha completa com '\n', mais '\0'. */
#define PROTO_LINE_BUF (RX_MAX_LINE + 1)

/* ---- Mensagens ---------------------------------------------------------- */

/* Começa em 1 de propósito: uma struct msg a zeros não é nenhum tipo válido. */
enum msg_type {
    MT_NICK = 1,    /* C -> S */
    MT_MSG,         /* C -> S */
    MT_OK,          /* S -> C */
    MT_CHAT,        /* S -> C */
    MT_ERROR,       /* S -> C */
    MT_EVENT        /* S -> C  (M2: entradas e saídas)                      */
};

/* Buffers fixos, sem malloc: logo sem free para esquecer. Só os campos do
 * `type` respetivo têm significado; os outros ficam a zeros. */
struct msg {
    enum msg_type type;
    char nick[PROTO_NICK_MAX + 1];     /* NICK                               */
    char from[PROTO_NICK_MAX + 1];     /* CHAT                               */
    char text[PROTO_TEXT_MAX + 1];     /* MSG, CHAT, EVENT                   */
    int  is_private;                   /* CHAT "private" (ausente = 0)       */
    int  code;                         /* ERROR, 100..999                    */
    char reason[PROTO_REASON_MAX + 1]; /* ERROR (ausente = "")               */
};

/* ---- Erros de parse ----------------------------------------------------- */

/* O que o servidor deve responder quando uma linha é recusada. O `reason` vai
 * para o fio tal e qual, por isso é em inglês, como o "nick taken" do slide. */
struct proto_err {
    int         code;      /* sempre 400 nesta camada                        */
    const char *reason;    /* literal estático — não libertar                */
};

#define PROTO_R_JSON  "invalid JSON"   /* não é JSON, lixo a seguir, '\0', não é objeto */
#define PROTO_R_TYPE  "unknown type"   /* "type" é string mas não é um tipo do M1       */
#define PROTO_R_FIELD "invalid field"  /* campo em falta, tipo errado ou fora dos limites */

/* ---- API ---------------------------------------------------------------- */

/* Converte uma linha (SEM o '\n') numa mensagem validada.
 *
 * `len` vem do framing (out_len) — não se usa strlen(), pela mesma razão que
 * lá: um '\0' embebido. Uma linha com '\0' é recusada.
 *
 * Devolve 0 e preenche `*out`; ou -1 e preenche `*err`. Em ambos os casos
 * `*out` é primeiro posto a zeros, por isso nunca fica meio preenchido.
 *
 * Campos desconhecidos são ignorados (PROTOCOL.md §3). Lixo a seguir ao
 * objeto ("{...}{...}") é recusado: aceitar o primeiro e ignorar o resto é
 * exatamente a falha silenciosa que o framing existe para evitar. */
int proto_parse(const char *line, size_t len, struct msg *out, struct proto_err *err);

/* Converte uma mensagem numa linha pronta a enviar: JSON compacto + '\n',
 * terminado em '\0' (que NÃO é para enviar).
 *
 *   out      pelo menos PROTO_LINE_BUF bytes
 *   out_len  bytes a enviar, INCLUINDO o '\n'. Pode ser NULL.
 *
 * Devolve 0, ou -1 se a mensagem não respeitar as regras acima, se out/cap
 * forem inválidos, ou se o cJSON falhar a alocar. -1 aqui é sempre "culpa
 * NOSSA" (como o RX_FAULT): nunca é causado pelo peer. */
int proto_encode(const struct msg *m, char *out, size_t cap, size_t *out_len);

/* As regras de conteúdo acima, expostas para o cliente validar o que o
 * utilizador escreve ANTES de tentar enviar (e dar uma mensagem de erro
 * concreta). 1 = válido. São as mesmas funções que o parse e o encode usam. */
int proto_valid_nick(const char *s);
int proto_valid_text(const char *s);

/* Nome do tipo no fio ("NICK", ...), ou "?" — para logs. */
const char *proto_type_name(enum msg_type t);

#endif /* PROTOCOL_H */
