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

#endif /* UI_H */
