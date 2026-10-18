/*
 * net.h — funções de sockets TCP partilhadas por cliente e servidor.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

/* Cria um socket à escuta em todas as interfaces, IPv4 E IPv6 (M3, slide 85):
 * um socket IPv6 com IPV6_V6ONLY=0 ("::"), que também aceita clientes IPv4.
 * Numa máquina sem IPv6 cai para IPv4 só ("0.0.0.0") e avisa no log.
 * Devolve o fd ou -1. */
int tcp_listen(const char *port, int backlog);

/* Liga a <host>:<port> (nome ou IP). Devolve o fd ou -1. */
int tcp_connect(const char *host, const char *port);

/* Envia os len bytes todos (send() pode enviar só parte). 0 ou -1.
 * Bloqueia o tempo que for preciso: usado pelo CLIENTE, que só escreve para
 * um peer (o servidor). */
int send_all(int fd, const void *buf, size_t len);

/* Como send_all(), mas com um PRAZO TOTAL de timeout_ms para enviar tudo.
 * Usado pelo SERVIDOR, que não pode ficar preso por um cliente que não lê.
 *
 *   0   enviado tudo
 *  -1   erro do socket (EPIPE, ECONNRESET...): o peer já foi
 *  -2   prazo esgotado: o peer não está a ler
 *
 * O prazo é para a linha INTEIRA e não por send(): um peer que vá libertando
 * uns bytes de vez em quando não consegue prolongar a espera (com SO_SNDTIMEO,
 * que é por chamada, conseguia).
 *
 * ATENÇÃO (typical-problems.md §1.3): com -1 ou -2 pode já ter saído PARTE da
 * linha. O stream desse peer fica corrompido sem remédio, por isso quem chama
 * TEM de deixar de escrever nesse socket e fechar a ligação. */
#define SEND_TIMEOUT_EXPIRED (-2)
int send_all_timeout(int fd, const void *buf, size_t len, int timeout_ms);

/* M4 — esperar por bytes com prazo, e acordar se o servidor encerrar.
 *
 * Espera que `fd` tenha alguma coisa para ler (bytes, FIN ou erro) até
 * timeout_ms (< 0 = sem prazo), ou que `stop_fd` fique legível (o pipe que o
 * servidor usa no Ctrl+C para acordar todas as threads; -1 = nenhum).
 *   NET_READY    fd legível: o recv() a seguir não bloqueia
 *   NET_TIMEOUT  passou o prazo sem nada
 *   NET_STOP     stop_fd legível (tem prioridade sobre o fd)
 *   -1           erro do poll()
 * Um sinal (EINTR) não acorda quem chama: o prazo continua a contar. */
#define NET_TIMEOUT 0
#define NET_READY   1
#define NET_STOP    2
int wait_readable(int fd, int stop_fd, long timeout_ms);

/* M4 — fechar sem que um RST engula a última resposta.
 *
 * Se fecharmos um socket com bytes do peer ainda por ler, o kernel manda RST
 * em vez de FIN, e o RST pode fazer o peer deitar fora o que lhe tínhamos
 * acabado de enviar (o ERROR 413, o OK ao QUIT, o 408...) antes de o ler
 * (M0 nota 4, typical-problems §2.2). Por isso: shutdown(SHUT_WR) (o nosso
 * FIN segue DEPOIS do que já está no buffer de envio) e ler e deitar fora o
 * que o peer mandar até ele fechar também, ou até timeout_ms.
 *   0  o peer fechou (o fecho final já não tem nada por ler: sai FIN)
 *   1  passou o prazo (o close() ainda pode sair como RST)
 *  -1  erro (o peer já tinha ido: RST)
 * NÃO fecha o fd: isso é de quem é dono dele (o refcount, no servidor). */
int linger_drain(int fd, long timeout_ms);

/* recv() que repete se for interrompido por um sinal (EINTR).
 * >0 bytes recebidos | 0 o outro lado fechou | -1 erro. */
ssize_t recv_retry(int fd, void *buf, size_t len);

/* Escreve "ip:porta" de um endereço em out: "1.2.3.4:5000" para IPv4 (e para
 * IPv4 que chegue por um socket dual-stack, ::ffff:1.2.3.4), "[::1]:5000"
 * para IPv6. */
void addr_to_str(const struct sockaddr *sa, char *out, size_t outlen);

#endif
