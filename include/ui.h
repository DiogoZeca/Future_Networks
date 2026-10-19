/*
 * ui.h — a parte do cliente que NÃO toca em sockets nem no terminal:
 * converter o que o utilizador escreve numa mensagem, e uma mensagem recebida
 * no texto a mostrar. Por ser pura, testa-se à unidade (tests/unit/test_ui.c),
 * como o framing, o protocol e a session.
 *
 * M3: os comandos do slide 123. O que começa por '/' é um comando:
 *
 *   /list               -> {"type":"LIST"}
 *   /msg <nick> <texto> -> {"type":"PRIV","to":"<nick>","text":"<texto>"}
 *   /quit               -> {"type":"QUIT"}
 *   /help               -> (local) mostrar os comandos
 *   //texto             -> MSG "/texto" (para enviar uma linha que começa por '/')
 *
 * Tudo o resto é um MSG para toda a gente, como no M2.
 *
 * REVISÃO DO M3: o text pode ter até 4096 bytes e caracteres de controlo
 * (mensagens de várias linhas). Por isso o ui_format() LIMPA o texto antes de
 * o mostrar: um '\033' mandado por outra pessoa podia apagar o ecrã ou mudar
 * as cores do terminal. Ficam o '\n' (as linhas seguintes aparecem
 * indentadas) e o '\t'; os outros caracteres de controlo aparecem como '?'.
 */
#ifndef UI_H
#define UI_H

#include <stddef.h>

#include "protocol.h"

/* Converte UMA linha escrita pelo utilizador (como vem do fgets, com ou sem
 * '\n' / "\r\n" no fim) numa mensagem.
 *
 * Devolve:
 *    UI_SEND (1)  *out é um MSG, PRIV, LIST ou QUIT pronto a codificar
 *    UI_NONE (0)  linha vazia — não enviar nada
 *    UI_HELP (2)  /help — mostrar UI_HELP_TEXT, não enviar nada
 *    UI_BAD (-1)  inválida; *why aponta para o motivo, em português
 *
 * As regras são as do protocolo (proto_valid_text, proto_valid_nick):
 * validar AQUI é o que permite dizer ao utilizador o que está mal, em vez de
 * um "não deu" genérico do proto_encode ou de um ERROR 400 do servidor. */
int ui_input_to_msg(const char *typed, struct msg *out, const char **why);

#define UI_SEND  1
#define UI_NONE  0
#define UI_HELP  2
#define UI_BAD  (-1)

#define UI_WHY_TOOLONG  "mensagem demasiado longa (maximo 4096 bytes)"
#define UI_WHY_UNKNOWN  "comando desconhecido (/help mostra os comandos)"
#define UI_WHY_ARGS     "este comando nao leva argumentos"
#define UI_WHY_MSG_USE  "uso: /msg <nick> <texto>"
#define UI_WHY_BADNICK  "nick invalido: 1 a 32 bytes, sem espacos, aspas, '\\' nem controlo"

#define UI_HELP_TEXT \
    "comandos:\n" \
    "  /list               quem esta ligado\n" \
    "  /msg <nick> <texto> mensagem privada\n" \
    "  /quit               sair (o mesmo que Ctrl+D)\n" \
    "  //texto             enviar uma linha que comeca por '/'\n" \
    "  qualquer outra linha vai para toda a gente"

/* Escreve em out (NUL-terminado, truncado se não couber) o texto a mostrar
 * para uma mensagem recebida do servidor:
 *
 *   CHAT    <ana> hello
 *   CHAT    <ana> (privado) hello            (um privado que nos chegou)
 *   CHAT    <ana -> rui> (privado) hello     (m->to preenchido: o eco LOCAL
 *                                             do nosso /msg, D14; nunca vem
 *                                             do fio)
 *   CHAT    <ana> linha 1                    (várias linhas: as seguintes
 *               linha 2                       indentadas)
 *   USERS   [online: 2] ana rui
 *   EVENT   * rui joined
 *   ERROR   [erro 401] nick required
 *   OK      [ok]   (o cliente não chega a mostrar os OK: ver client/main.c)
 *   outros  [?]
 * Um text/reason/USERS que o protocolo teve de cortar (m->truncated) acaba
 * em " [...]". */
void ui_format(const struct msg *m, char *out, size_t cap);

/* ---- Editor de linha (correção do M3) -----------------------------------
 *
 * Até aqui era o TERMINAL que mostrava o que se escrevia (modo canónico): o
 * programa não sabia o que estava no ecrã, e daí três problemas vistos no
 * teste cruzado — uma mensagem que chegasse a meio da escrita ficava colada
 * ao que se estava a escrever; o que se enviava aparecia duas vezes (o eco do
 * terminal e o CHAT do servidor); e cada Enter sozinho fazia subir o ecrã.
 *
 * Agora, com stdin num terminal, o cliente lê tecla a tecla (term.c) e guarda
 * ele próprio a linha numa `struct ui_line`. A parte que decide o que cada
 * tecla faz está aqui, pura, para se testar sem terminal (test_ui.c #85-#87).
 *
 * Teclas:
 *   texto (UTF-8 incluído)  acrescenta (um carácter UTF-8 só aparece quando
 *                           chegaram todos os seus bytes; bytes soltos saem)
 *   Backspace (DEL ou ^H)   apaga um CARÁCTER (todos os bytes de um "é")
 *   Ctrl+U                  apaga a linha toda
 *   Ctrl+W                  apaga a última palavra
 *   Ctrl+L                  volta a desenhar
 *   Enter                   com texto: enviar; com a linha vazia: NADA
 *   Ctrl+D                  com a linha vazia: sair (como o EOF de antes)
 *   ESC [ ... / ESC O x     (setas, Home, Delete, F1...) ignoradas inteiras
 *   outro controlo, Tab     ignorados
 */

/* "/msg " + nick + ' ' + texto: a linha mais longa que pode dar uma mensagem
 * válida. Mais do que isto nunca seria aceite, por isso não se deixa
 * escrever (UI_KEY_FULL). */
#define UI_LINE_MAX (PROTO_TEXT_MAX + PROTO_NICK_MAX + 6)

struct ui_line {
    char   buf[UI_LINE_MAX + 1];   /* sempre NUL-terminado */
    size_t len;                    /* bytes em buf */
    int    esc;                    /* 0 normal, 1 depois de ESC, 2 numa sequência */
    int    drop;                   /* bytes de continuação a deitar fora (um
                                      carácter UTF-8 que já não coube) */
    int    want;                   /* bytes que faltam ao carácter a meio */
    int    part;                   /* bytes que ele já tem no fim de buf */
};

void ui_line_init(struct ui_line *l);

/* Trata UM byte vindo do teclado. Devolve o que o chamador tem de fazer: */
#define UI_KEY_NONE   0   /* nada mudou no ecrã                              */
#define UI_KEY_REDRAW 1   /* a linha mudou: voltar a desenhá-la              */
#define UI_KEY_SUBMIT 2   /* Enter com texto: l->buf é a linha; o chamador
                             trata-a e faz ui_line_init()                    */
#define UI_KEY_EOF    3   /* Ctrl+D com a linha vazia: sair                  */
#define UI_KEY_FULL   4   /* não coube (a linha fica igual): avisar          */
int ui_line_key(struct ui_line *l, unsigned char c);

/* Onde começa (em bytes) a parte da linha que cabe em `cols` colunas — o fim
 * da linha, que é onde se está a escrever. Conta um carácter por coluna (os
 * caracteres "largos", como emojis, ficam desalinhados: limitação aceite) e
 * nunca corta um carácter UTF-8 a meio. cols == 0 → nada cabe. */
size_t ui_line_tail(const struct ui_line *l, size_t cols);

/* Até onde (em bytes) a linha se pode mostrar: sem o carácter UTF-8 que está
 * a meio de chegar (meio carácter no ecrã é lixo). O ecrã mostra
 * buf[ui_line_tail(l, cols) .. ui_line_shown(l)). */
size_t ui_line_shown(const struct ui_line *l);

/* ---- M5: escolher um servidor da descoberta ---------------------------- */

/* O que o utilizador escreveu quando a descoberta encontrou `n` servidores e
 * se lhe pediu um número: "2", " 2 ", "2\n" -> 2. Devolve 1..n, ou 0 se não
 * for um número nesse intervalo ("", "0", "x", "2x", "-1", n+1...). */
int ui_parse_choice(const char *typed, size_t n);

#endif /* UI_H */
