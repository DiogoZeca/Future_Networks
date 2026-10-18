/*
 * term.h — o ecrã e o teclado do cliente (correção do M3).
 *
 * O PROBLEMA (visto no teste cruzado com o grupo 1)
 * -------------------------------------------------
 * Com o terminal em modo "canónico" (o normal), quem mostra o que se escreve
 * é o TERMINAL, e o programa só recebe a linha no Enter. Ao mesmo tempo, a
 * thread de leitura escreve no ecrã quando chega uma mensagem. O programa não
 * sabe o que está no ecrã, e daí:
 *
 *   - escrever "ola tu" e chegar um CHAT dava "ola tu<rui> boas";
 *   - o que se enviava aparecia DUAS vezes: o eco do terminal ("ola") e o
 *     CHAT que o servidor nos devolve ("<ana> ola", D5);
 *   - cada Enter sozinho deixava uma linha em branco e o chat subia.
 *
 * A SOLUÇÃO
 * ---------
 * Com stdin e stdout num terminal, o cliente põe-no em modo "raw" (sem
 * ICANON nem ECHO), lê tecla a tecla e guarda ele próprio a linha
 * (ui_line_*, em ui.c, testada à unidade). A linha de escrita é sempre a
 * última do ecrã, com o nick à frente:
 *
 *     <rui> boas
 *     * zed joined
 *     ana> estou a escre_
 *
 * Quem quiser mostrar alguma coisa chama term_out(): apaga a linha de
 * escrita, mostra a mensagem e volta a desenhar o prompt com o que se estava
 * a escrever. No Enter a linha de escrita é apagada antes de enviar — o que
 * fica no ecrã é o CHAT do servidor (ou o eco local do /msg), uma vez só.
 *
 * Duas threads escrevem no ecrã e a linha de escrita é partilhada: há UM
 * mutex (o do ecrã) à volta de cada term_out() e de cada tecla tratada.
 * Nunca se fica com ele durante um read() ou um recv().
 *
 * Sem terminal (stdin de um pipe ou de um ficheiro, como no probe_m3.py §7)
 * fica tudo como antes: fgets() e printf(), sem prompt.
 *
 * O terminal é SEMPRE reposto como estava: no fim normal, no exit() da
 * thread de leitura (atexit), e nos sinais que matam o processo (Ctrl+C,
 * Ctrl+\, SIGTERM, SIGHUP). Com Ctrl+Z é reposto ao suspender e volta a raw
 * ao continuar (fg). Sem isto, a shell ficava sem eco depois de o cliente
 * sair. (O Ctrl+C continua a terminar o cliente sem QUIT: isso é do M4.)
 */
#ifndef TERM_H
#define TERM_H

#include <stddef.h>
#include <stdio.h>

/* Se stdin e stdout forem um terminal, passa ao modo raw com o prompt
 * "<nick>> " e devolve 1; senão não mexe em nada e devolve 0. */
int term_start(const char *nick);

/* Repõe o terminal (idempotente; seguro de chamar de um atexit). A partir
 * daqui term_out() escreve sem prompt. */
void term_stop(void);

/* Os sinais que o terminal trata têm de chegar à thread PRINCIPAL (é ela que
 * está no read() do teclado e redesenha depois de um Ctrl+Z ou de mudar o
 * tamanho da janela). Bloquear antes de criar a thread de leitura (que herda
 * a máscara) e desbloquear a seguir. */
void term_hold_signals(void);
void term_release_signals(void);

/* Mostra UMA mensagem (sem '\n' no fim; pode ter '\n' no meio) em `f`
 * (stdout ou stderr), sem a misturar com a linha que se está a escrever.
 * Seguro de chamar de qualquer thread. */
void term_out(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Lê uma linha do teclado para out (cap >= UI_LINE_MAX + 3, NUL-terminada).
 *   TERM_LINE     uma linha (sem terminal pode trazer o '\n')
 *   TERM_EOF      Ctrl+D com a linha vazia, ou fim do stdin
 *   TERM_TOOLONG  (só sem terminal) a linha não cabia e foi deitada fora
 *   TERM_ERR      erro de leitura
 * Com terminal, uma linha vazia nunca é devolvida (o Enter sozinho não faz
 * nada) e a linha nunca passa de UI_LINE_MAX. */
#define TERM_LINE     1
#define TERM_EOF      0
#define TERM_TOOLONG  2
#define TERM_ERR     (-1)
int term_readline(char *out, size_t cap);

#endif /* TERM_H */
