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
 *   - Os nicks deixam de poder ter '"' e '\\' (D20). Não é estética: o JSON
 *     escapa-os com 2 bytes, e um USERS com 64 nicks de 32 bytes cheios de
 *     aspas não caberia numa linha de 4096 (o limite até à revisão do M3).
 *   - Regra de nicks em duas versões (estrito a enviar, tolerante a receber):
 *     o que NÓS aceitamos num NICK/PRIV e enviamos é a regra estrita; o que
 *     um servidor de OUTRO grupo nos mande num CHAT/USERS só tem de ser
 *     partível por espaços (1..32 bytes, sem espaços nem controlo).
 *   - proto_nick_eq(): a ÚNICA definição de "é o mesmo nick" (D18), usada
 *     pelo roster (nicks únicos) e pela session (destino de um PRIV).
 *
 * REVISÃO DO M3 (acordo com o primeiro grupo, M3.md "Revisão do M3")
 * ------------------------------------------------------------------
 *   - Linha até 64 KiB (framing.h); text até 4096 bytes, com mensagens de
 *     VÁRIAS LINHAS: o text já pode ter caracteres de controlo ('\n', '\t'...).
 *     O JSON escapa-os, por isso o enquadramento continua seguro.
 *   - text acima de 4096 bytes num pedido -> ERROR 400 "text too long" (a
 *     ligação continua); text vazio continua recusado ("invalid field").
 *   - O CHAT perdeu o campo "to" (a cópia do PRIV ao emissor acabou): volta a
 *     ter exatamente os campos do slide.
 *   - Tolerante a receber: um CHAT/EVENT de outro servidor com text vazio ou
 *     maior do que 4096 é aceite (cortado aos 4096, sem partir um caracter
 *     UTF-8 a meio, e marcado em `truncated`), em vez de ser recusado inteiro.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#include "framing.h"   /* RX_MAX_LINE */

/* ---- Limites (D4, revistos no M3 com o primeiro grupo) ------------------- */

#define PROTO_NICK_MAX   32     /* bytes, não caracteres                     */
/* Um nick que vem de OUTRO servidor (CHAT.from, USERS) pode ter até 128
 * bytes: há grupos que contam o limite de 32 em CARACTERES, e 32 caracteres
 * UTF-8 são até 4 x 32 = 128 bytes. O nosso servidor continua nos 32 bytes
 * (tolerante a receber, estrito a enviar). */
#define PROTO_PEER_NICK_MAX 128
#define PROTO_TEXT_MAX   4096   /* bytes (era 1024 até à revisão do M3)      */
#define PROTO_REASON_MAX 128    /* bytes, o "reason" de um ERROR             */

/* Regras de conteúdo (validadas no parse E no encode — nunca enviamos o que
 * recusaríamos):
 *   - nick:   1..32 bytes, sem espaços, sem '"' nem '\\' (D20) e sem
 *             caracteres de controlo (sem espaços porque o `/msg <nick> texto`
 *             parte por espaço, e o USERS guarda os nicks separados por um)
 *   - text:   1..4096 bytes. Pode ter caracteres de controlo, ou seja,
 *             mensagens de VÁRIAS LINHAS (revisão do M3). Quem MOSTRA o texto
 *             é que o limpa (ui.c), para um '\033' não mexer no terminal.
 *   - reason: 0..128 bytes, sem caracteres de controlo
 * "Caracter de controlo" = byte < 0x20 ou 0x7F. UTF-8 não é validado (D3).
 *
 * O que vem do SERVIDOR é aceite com regras largas (tolerante a receber):
 *   - nicks (CHAT.from, USERS.nicks): 1..128 bytes (PROTO_PEER_NICK_MAX),
 *     sem espaços nem controlo — '"' e '\\' passam, para não recusar um CHAT
 *     inteiro de um servidor que decidiu diferente;
 *   - text (CHAT, EVENT) e reason (ERROR): qualquer tamanho, vazio incluído;
 *     o que passar do buffer é cortado e marcado em `truncated`. */

/* Quantos nicks pode ter um USERS que NÓS enviamos. O roster.h verifica que
 * ROSTER_MAX não passa disto. (A receber não há limite de contagem: guarda-se
 * o que couber em PROTO_NICKS_BUF e marca-se `truncated`.) */
#define PROTO_USERS_MAX  64

/* Porque é que o enquadramento continua seguro com '\n' no text: o cJSON
 * escapa cada caracter de controlo com no máximo 6 bytes ("\u0001"; o '\n'
 * fica "\n", 2 bytes) e '"' e '\\' com 2. Nenhum byte cresce mais de 6x, por
 * isso o pior caso tem um tamanho conhecido, e nunca há um '\n' literal no
 * JSON. Os nicks (D20) não crescem nada.
 *
 * Os dois piores casos (testes #27 e #59 executam-nos):
 *
 *   CHAT com from no limite e text no limite cheio de '\001':
 *     {"type":"CHAT","from":"","text":"","private":false}\n = 52 fixos
 *     + 32 + 6*4096                                          = 24660 bytes
 *
 *   USERS com 64 nicks de 32 bytes:
 *     {"type":"USERS","nicks":[]}\n = 28 fixos
 *     + 64 * (32 + 2 aspas) + 63 vírgulas                    = 2267 bytes
 *
 * Os _Static_assert garantem que ambos cabem numa linha do framing — se alguém
 * subir os limites sem pensar, não compila. */
#define PROTO_CHAT_OVERHEAD  52
#define PROTO_USERS_OVERHEAD 28
#define PROTO_ESCAPE_MAX     6      /* bytes por caracter, no pior escape */
_Static_assert(PROTO_NICK_MAX + PROTO_ESCAPE_MAX * PROTO_TEXT_MAX + PROTO_CHAT_OVERHEAD
                   <= RX_MAX_LINE,
               "o pior CHAT codificado tem de caber numa linha do framing");
_Static_assert(PROTO_USERS_MAX * (PROTO_NICK_MAX + 3) + PROTO_USERS_OVERHEAD
                   <= RX_MAX_LINE,
               "o pior USERS codificado tem de caber numa linha do framing");

/* Os nicks de um USERS guardam-se numa só string, separados por UM espaço
 * ("ana rui zed") — os nicks não têm espaços, por isso não há ambiguidade.
 * 8 KB chegam para ~250 nicks no limite; um USERS recebido maior (um servidor
 * federado com muita gente) é guardado até aqui e marcado `truncated`. Não é
 * RX_MAX_LINE (64 KiB) de propósito: a struct msg vive na stack, várias de
 * cada vez. */
#define PROTO_NICKS_BUF  8192
_Static_assert(PROTO_USERS_MAX * (PROTO_NICK_MAX + 1) <= PROTO_NICKS_BUF,
               "um USERS nosso tem de caber inteiro no buffer");

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
    char from[PROTO_PEER_NICK_MAX + 1]; /* CHAT (de outro servidor: até 128) */
    char to[PROTO_NICK_MAX + 1];       /* PRIV. Num CHAT NÃO vai para o fio: */
                                       /* só encaminha (SESS_TO) e mostra o  */
                                       /* eco local do /msg (ui.c)           */
    char text[PROTO_TEXT_MAX + 1];     /* MSG, PRIV, CHAT, EVENT             */
    int  is_private;                   /* CHAT "private" (ausente = 0)       */
    int  code;                         /* ERROR, 100..999                    */
    char reason[PROTO_REASON_MAX + 1]; /* ERROR (ausente = "")               */
    char   nicks[PROTO_NICKS_BUF];     /* USERS: "ana rui zed" (ver acima)   */
    size_t n_nicks;                    /* USERS: quantos guardados           */
    int    truncated;                  /* recebido: text/reason/nicks cortado */
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
#define PROTO_R_TEXT_LONG "text too long" /* text de um pedido > PROTO_TEXT_MAX (M3)  */

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

/* Os len bytes são UTF-8 válido (estrito: sem overlong, sem surrogates, nada
 * acima de U+10FFFF)? 1 = sim. É a verificação que o proto_parse() faz a cada
 * linha (M4, P38); exposta no M5 para os datagramas da descoberta (disco.c)
 * usarem a MESMA regra. */
int proto_utf8_valid(const char *s, size_t len);

/* Nome do tipo no fio ("NICK", ...), ou "?" — para logs. */
const char *proto_type_name(enum msg_type t);

#endif /* PROTOCOL_H */
