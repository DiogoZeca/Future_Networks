/*
 * ui.h — a parte do cliente que NÃO toca em sockets nem no terminal:
 * converter o que o utilizador escreve numa mensagem, e uma mensagem recebida
 * no texto a mostrar. Por ser pura, testa-se à unidade (tests/unit/test_ui.c),
 * como o framing, o protocol e a session.
 */
#ifndef UI_H
#define UI_H

#include <stddef.h>

#include "protocol.h"

/* Converte UMA linha escrita pelo utilizador (como vem do fgets, com ou sem
 * '\n' / "\r\n" no fim) num MSG.
 *
 * Devolve:
 *    1  *out é um MSG pronto a codificar
 *    0  linha vazia — não enviar nada
 *   -1  inválida; *why aponta para o motivo, em português, para mostrar
 *
 * As regras são as do protocolo (proto_valid_text): validar AQUI é o que
 * permite dizer ao utilizador o que está mal, em vez de um "não deu" genérico
 * do proto_encode. */
int ui_input_to_msg(const char *typed, struct msg *out, const char **why);

#define UI_WHY_TOOLONG "mensagem demasiado longa (maximo 1024 bytes)"
#define UI_WHY_CONTROL "a mensagem tem caracteres de controlo (ex.: tab)"

/* Escreve em out (NUL-terminado, truncado se não couber) o texto a mostrar
 * para uma mensagem recebida do servidor:
 *
 *   CHAT    <ana> hello
 *   CHAT    <ana> (privado) hello
 *   ERROR   [erro 401] nick required
 *   OK      [ok]
 *   outros  [?]
 */
void ui_format(const struct msg *m, char *out, size_t cap);

#endif /* UI_H */
