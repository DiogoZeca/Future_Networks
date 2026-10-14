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
 * Tipos do M1 (slide 123: NICK e MSG, mais as respostas OK, CHAT e ERROR),
 * EVENT desde o M2 ("join and leave notices") e, no M3, os restantes do slide
 * 124: PRIV, LIST e QUIT (C -> S) e USERS (S -> C). Com eles a tabela do slide
 * fica completa: nenhum tipo do slide é "unknown type".
 *
 *
 * O QUE MUDOU DO M2 PARA O M3
 * ---------------------------
 *   - 4 tipos novos; o USERS traz o primeiro ARRAY do protocolo ("nicks").
 *   - CHAT ganha um campo opcional "to" (extensão nossa, D14): numa mensagem
 *     privada diz para quem foi. Quem não o conhece ignora-o (§3 do
 *     PROTOCOL.md: campos desconhecidos são ignorados).
 *   - Os nicks deixam de poder ter '"' e '\\' (D20). Não é estética: o JSON
 *     escapa-os com 2 bytes, e um USERS com 64 nicks de 32 bytes cheios de
 *     aspas daria ~4315 bytes — mais do que uma linha (4096). Ver o
 *     _Static_assert do USERS mais abaixo.
 *   - Regra de nicks em duas versões (estrito a enviar, tolerante a receber):
 *     o que NÓS aceitamos num NICK/PRIV e enviamos é a regra estrita; o que
 *     um servidor de OUTRO grupo nos mande num CHAT/USERS só tem de ser
 *     partível por espaços (1..32 bytes, sem espaços nem controlo).
 *   - proto_nick_eq(): a ÚNICA definição de "é o mesmo nick" (D18), usada
 *     pelo roster (nicks únicos) e pela session (destino de um PRIV).
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
 *   - nick:   1..32 bytes, sem espaços, sem '"' nem '\\' (D20) e sem
 *             caracteres de controlo (sem espaços porque o `/msg <nick> texto`
 *             parte por espaço, e o USERS guarda os nicks separados por um)
 *   - text:   1..1024 bytes, sem caracteres de controlo (logo sem '\n':
 *             mensagens de várias linhas não são suportadas)
 *   - reason: 0..128 bytes, sem caracteres de controlo
 * "Caracter de controlo" = byte < 0x20 ou 0x7F. UTF-8 não é validado (D3).
 *
 * Nicks que vêm do SERVIDOR (CHAT.from, CHAT.to, USERS.nicks) são aceites com
 * a regra larga: 1..32 bytes, sem espaços nem controlo — '"' e '\\' passam,
 * para não recusar um CHAT inteiro de um servidor que decidiu diferente. */

/* Quantos nicks pode ter um USERS que NÓS enviamos. O roster.h verifica que
 * ROSTER_MAX não passa disto. (A receber não há limite de contagem: cabe o que
 * couber numa linha.) */
#define PROTO_USERS_MAX  64

/* Porque é que recusar caracteres de controlo também garante o enquadramento:
 * o cJSON escapa '"' e '\\' com 2 bytes e os caracteres de controlo com até 6
 * ("\u0001"). Sem estes últimos, nenhum byte cresce mais de 2x ao codificar, e
 * o pior caso tem um tamanho conhecido. E desde o D20 os nicks nem isso: não
 * têm '"' nem '\\', logo não crescem nada.
 *
 * Os dois piores casos (testes #27 e #59 executam-nos):
 *
 *   CHAT privado, from/to no limite e text no limite cheio de '"':
 *     {"type":"CHAT","from":"","to":"","text":"","private":false}\n = 60 fixos
 *     + 32 + 32 + 2*1024                                     = 2172 bytes
 *
 *   USERS com 64 nicks de 32 bytes:
 *     {"type":"USERS","nicks":[]}\n = 28 fixos
 *     + 64 * (32 + 2 aspas) + 63 vírgulas                    = 2267 bytes
 *     (com aspas nos nicks seria 64 * (64 + 2) + 63 + 28 = 4315: NÃO cabia)
 *
 * Os _Static_assert garantem que ambos cabem numa linha do framing — se alguém
 * subir os limites sem pensar, não compila. */
#define PROTO_CHAT_OVERHEAD  60
#define PROTO_USERS_OVERHEAD 28
_Static_assert(2 * PROTO_NICK_MAX + 2 * PROTO_TEXT_MAX + PROTO_CHAT_OVERHEAD
                   <= RX_MAX_LINE,
               "o pior CHAT codificado tem de caber numa linha do framing");
_Static_assert(PROTO_USERS_MAX * (PROTO_NICK_MAX + 3) + PROTO_USERS_OVERHEAD
                   <= RX_MAX_LINE,
               "o pior USERS codificado tem de caber numa linha do framing");

/* Os nicks de um USERS guardam-se numa só string, separados por UM espaço
 * ("ana rui zed") — os nicks não têm espaços, por isso não há ambiguidade.
 * Um USERS recebido nunca passa de uma linha, e cada nick ocupa lá pelo menos
 * mais 2 bytes (aspas) do que aqui (1 espaço): RX_MAX_LINE chega sempre. */
#define PROTO_NICKS_BUF  RX_MAX_LINE

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
    MT_EVENT,       /* S -> C  (M2: entradas e saídas)                      */
    MT_PRIV,        /* C -> S  (M3: mensagem privada, /msg)                 */
    MT_LIST,        /* C -> S  (M3: pedir a lista, /list)                   */
    MT_QUIT,        /* C -> S  (M3: sair de forma limpa, /quit)             */
    MT_USERS        /* S -> C  (M3: resposta ao LIST)                       */
};

/* Buffers fixos, sem malloc: logo sem free para esquecer. Só os campos do
 * `type` respetivo têm significado; os outros ficam a zeros. */
struct msg {
    enum msg_type type;
    char nick[PROTO_NICK_MAX + 1];     /* NICK                               */
    char from[PROTO_NICK_MAX + 1];     /* CHAT                               */
    char to[PROTO_NICK_MAX + 1];       /* PRIV; CHAT "to" (opcional, D14)    */
    char text[PROTO_TEXT_MAX + 1];     /* MSG, PRIV, CHAT, EVENT             */
    int  is_private;                   /* CHAT "private" (ausente = 0)       */
    int  code;                         /* ERROR, 100..999                    */
    char reason[PROTO_REASON_MAX + 1]; /* ERROR (ausente = "")               */
    char   nicks[PROTO_NICKS_BUF];     /* USERS: "ana rui zed" (ver acima)   */
    size_t n_nicks;                    /* USERS: quantos                     */
};

/* ---- Erros de parse ----------------------------------------------------- */

/* O que o servidor deve responder quando uma linha é recusada. O `reason` vai
 * para o fio tal e qual, por isso é em inglês, como o "nick taken" do slide. */
struct proto_err {
    int         code;      /* sempre 400 nesta camada                        */
    const char *reason;    /* literal estático — não libertar                */
};

#define PROTO_R_JSON  "invalid JSON"   /* não é JSON, lixo a seguir, '\0', não é objeto */
#define PROTO_R_TYPE  "unknown type"   /* "type" é string mas não é um tipo do slide 124 */
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

/* "São o mesmo nick?" — a ÚNICA definição (D18, PROPOSTA). Compara ignorando
 * maiúsculas em ASCII ("Ana" == "ana" == "ANA"); bytes >= 0x80 (UTF-8)
 * comparam-se tal e qual ("Zé" != "ZÉ"). Usada para os nicks únicos (409) e
 * para encontrar o destino de um PRIV. Mudar a regra é mudar só isto. */
int proto_nick_eq(const char *a, const char *b);

/* Ordem para o USERS (D16): a mesma dobragem de maiúsculas e, em empate, os
 * bytes. Tipo strcmp: <0, 0, >0. */
int proto_nick_cmp(const char *a, const char *b);

/* Acrescenta um nick a um USERS (m->type tem de ser MT_USERS). Devolve 0, ou
 * -1 se o nick for inválido ou já houver PROTO_USERS_MAX. */
int proto_users_add(struct msg *m, const char *nick);

/* Nome do tipo no fio ("NICK", ...), ou "?" — para logs. */
const char *proto_type_name(enum msg_type t);

#endif /* PROTOCOL_H */
