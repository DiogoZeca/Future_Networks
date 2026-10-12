/*
 * net.h — funções de sockets TCP partilhadas por cliente e servidor.
 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

/* Cria um socket à escuta em 0.0.0.0:<port>. Devolve o fd ou -1. */
int tcp_listen(const char *port, int backlog);

/* Liga a <host>:<port> (nome ou IP). Devolve o fd ou -1. */
int tcp_connect(const char *host, const char *port);

/* Envia os len bytes todos (send() pode enviar só parte). 0 ou -1. */
int send_all(int fd, const void *buf, size_t len);

/* recv() que repete se for interrompido por um sinal (EINTR).
 * >0 bytes recebidos | 0 o outro lado fechou | -1 erro. */
ssize_t recv_retry(int fd, void *buf, size_t len);

/* Escreve "ip:porta" de um endereço em out. */
void addr_to_str(const struct sockaddr *sa, char *out, size_t outlen);

#endif
