/*
 * log.h — o log do servidor (M4, slide 123: "logging").
 *
 * Até ao M3 o servidor fazia printf()/fprintf() soltos, com o prefixo
 * "[server]": sem hora, sem nível, metade no stdout e metade no stderr. Para
 * perceber o que aconteceu a uma ligação (porque é que saiu? quando?) era
 * preciso adivinhar a ordem das linhas.
 *
 * Agora cada linha é:
 *
 *     2026-10-18 10:23:45.123 INFO  127.0.0.1:51234 (ana) saiu: inativo ha 300 s (408)
 *     ^ data e hora local, ms   ^ nível
 *
 *   - UMA linha por evento, escrita de uma vez (um fwrite com o stderr
 *     trancado): as threads nunca intercalam meia linha com meia linha.
 *   - Tudo no stderr, sem buffer: o que está no log já aconteceu, mesmo que o
 *     processo morra a seguir.
 *   - Níveis: ERROR (bug nosso, ou o sistema a falhar), WARN (alguém a
 *     portar-se mal: lista cheia, cliente que não lê), INFO (entradas, saídas
 *     e porquê, erros enviados, arranque e encerramento), DEBUG (cada
 *     mensagem recebida). Por omissão INFO; -v no servidor liga o DEBUG, -q
 *     deixa só WARN e ERROR.
 *
 * O texto das mensagens não vai para o log em INFO (só em DEBUG, e cortado):
 * o log diz QUEM e PORQUÊ, não O QUÊ.
 */
#ifndef LOG_H
#define LOG_H

#include <stddef.h>
#include <time.h>

enum log_level { LV_ERROR = 0, LV_WARN, LV_INFO, LV_DEBUG };

/* Só as linhas com nível <= este aparecem. Por omissão LV_INFO. */
void log_set_level(enum log_level lv);
int  log_enabled(enum log_level lv);

/* Uma linha de log (sem '\n' no fim). Seguro de chamar de qualquer thread. */
void log_msg(enum log_level lv, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_error(...) log_msg(LV_ERROR, __VA_ARGS__)
#define log_warn(...)  log_msg(LV_WARN,  __VA_ARGS__)
#define log_info(...)  log_msg(LV_INFO,  __VA_ARGS__)
#define log_debug(...) log_msg(LV_DEBUG, __VA_ARGS__)

/* A parte pura, para os testes (test_log.c): escreve em out a linha completa
 * (com '\n') para o instante `when` (tempo real) e devolve o tamanho. Uma
 * mensagem que não caiba é cortada e acaba em "...\n". Nunca escreve além
 * de cap; cap tem de ser >= 64. */
size_t log_format(char *out, size_t cap, enum log_level lv,
                  const struct timespec *when, const char *msg);

#endif /* LOG_H */
