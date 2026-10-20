/*
 * tlsio.h — a E/S de UMA ligação, com ou sem TLS.
 *
 * O chat fala por linhas; quem as transporta é esta camada. Em claro, cada
 * função aqui é a de net.c que já existia — a indirecção não muda
 * comportamento nenhum. Com TLS (M6), as mesmas operações passam por um SSL*.
 *
 *   framing/protocol  <-- linhas -->  [ tlsio ]  <-- bytes -->  socket
 *                                        |
 *                                  net.c  ou  OpenSSL
 *
 * É isto que permite que o src/server/main.c e o src/client/main.c não tenham
 * um único `if (tls)`: recebem um `struct conn_io *` e tratam todas as
 * ligações da mesma maneira.
 *
 *
 * PORQUE É QUE ISTO NÃO É SÓ "EMBRULHAR O SOCKET"
 * -----------------------------------------------
 * Em TCP os dois sentidos de uma ligação são independentes: o kernel trata
 * deles em separado, e é por isso que até aqui o servidor podia ler numa
 * thread (recv_retry) enquanto outras escreviam (send_all_timeout, com o
 * wlock só entre elas), e o cliente não precisava de lock nenhum.
 *
 * Um SSL* NÃO é assim. É um objeto único com estado partilhado pelos dois
 * sentidos (records, números de sequência, renegociação), e a documentação do
 * OpenSSL é explícita: não pode ser usado por duas threads ao mesmo tempo sem
 * sincronização externa.
 *
 * Por isso, a partir do M6, com TLS o lock de cada ligação cobre LEITURA e
 * ESCRITA — e nenhuma chamada bloqueia com ele agarrado: depois do handshake
 * o socket é não-bloqueante, um record a meio dá IO_AGAIN, e a espera é no
 * io_wait_readable(), fora do lock. Ver docs/milestones/M6.md §D-TLS-3.
 *
 * Esta camada não tem locks seus: serializar é com quem chama (o wlock de
 * cada cliente no servidor, o ssl_lock no cliente). A única que mexe num lock
 * é o io_send_all_shared(), e é o de quem chama, que ela larga para esperar.
 */
#ifndef TLSIO_H
#define TLSIO_H

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>

#include "net.h"   /* NET_READY, NET_TIMEOUT, NET_STOP, SEND_TIMEOUT_EXPIRED */

/*
 * Pública e por valor, não opaca: o `struct client` do roster embebe-a, e
 * assim não há uma alocação (nem um free para esquecer) por ligação.
 *
 * `ssl` é void* de propósito: quem usa esta camada não precisa de incluir os
 * headers do OpenSSL, e o caminho em claro não depende deles de todo.
 */
struct conn_io {
    int   fd;
    void *ssl;    /* SSL*, ou NULL quando a ligação é em claro */
};

/* Uma ligação em claro sobre `fd`. fd = -1 é válido (os testes do roster). */
void io_init_plain(struct conn_io *io, int fd);

/* ---- TLS: o contexto (M6) ----------------------------------------------- */

/* Um SSL_CTX de servidor com o certificado e a chave de `cert` e `key`, TLS
 * 1.2 no mínimo. É criado UMA vez no arranque e partilhado por todas as
 * ligações — ao contrário do SSL*, um SSL_CTX pode ser usado por várias
 * threads.
 *
 * Devolve o contexto (void*: quem chama não precisa dos headers do OpenSSL),
 * ou NULL com o motivo em *why (string estática; pode ser NULL). */
void *tls_server_ctx(const char *cert, const char *key, const char **why);

/* Um SSL_CTX de cliente. NÃO valida a cadeia — o certificado é auto-assinado,
 * não há autoridade nenhuma. Quem autentica o servidor é o fingerprint, no
 * io_tls_connect(). Ver M6.md §D-TLS-2. */
void *tls_client_ctx(const char **why);

void tls_ctx_free(void *ctx);

/* O SHA-256 do certificado do peer, em hexadecimal com ':' ("1E:C4:..."),
 * ou "" se não houver. Para os logs e para quem quer anotar o fingerprint. */
void io_peer_fingerprint(const struct conn_io *io, char *out, size_t cap);

/* ---- TLS: handshake ----------------------------------------------------- */

/* Handshake do lado do SERVIDOR sobre um fd já aceite, com prazo.
 *
 * Tem prazo porque um cliente que liga e não fala deixaria a thread presa —
 * e, se isto fosse feito no ciclo do accept(), prendia o servidor inteiro. É
 * por isso que quem chama é a thread do cliente, nunca o accept(). M6.md §3.3.
 *
 * 0 = ligação cifrada pronta (io preenchido); -1 = falhou, com o motivo em
 * *why. Em qualquer dos casos o fd NÃO é fechado: é de quem chama. */
int io_tls_accept(struct conn_io *io, int fd, void *ctx, int timeout_ms,
                  const char **why);

/* Handshake do lado do CLIENTE, com autenticação por fingerprint.
 *
 * `fp` é o SHA-256 do certificado do servidor em hexadecimal, com ou sem ':'
 * e em maiúsculas ou minúsculas ("1E:C4:..." ou "1ec4..."). É o que o
 * scripts/gen-cert.sh imprime.
 *
 *   fp != NULL   o certificado tem de bater certo. Se não bater, a ligação é
 *                recusada ANTES de sair uma única mensagem nossa.
 *   fp == NULL   não se verifica nada: cifra SEM autenticar. Um atacante no
 *                meio apresenta o certificado dele e é aceite. Só serve para
 *                testes, e quem chama deve avisar o utilizador.
 *
 * Porquê isto e não a validação normal da cadeia: o certificado é
 * auto-assinado, não há autoridade nenhuma, e qualquer verificação de cadeia
 * falharia sempre. O fingerprint é o mesmo modelo do SSH (M6.md §D-TLS-2).
 *
 * 0 = ligação cifrada e autenticada; -1 = falhou, com o motivo em *why. */
int io_tls_connect(struct conn_io *io, int fd, void *ctx, const char *fp,
                   int timeout_ms, const char **why);

/* 1 se `fp` for um fingerprint SHA-256 aceitável (64 dígitos hex, com ou sem
 * ':'). Para o cliente validar o que lhe deram na linha de comandos antes de
 * sequer ligar. */
int tls_valid_fingerprint(const char *fp);

/* O *why que o io_tls_connect() devolve quando foi o FINGERPRINT a falhar (e
 * não o handshake). Ponteiro estático: compara-se com == . Serve para quem
 * chama dar uma dica certa em vez de uma dica para todos os erros. */
extern const char TLS_WHY_FINGERPRINT[];

/* 1 se a ligação for cifrada. Para os logs e para o `/help`: a lógica do chat
 * não deve precisar de saber. */
int io_is_tls(const struct conn_io *io);

/* O descritor, para o poll() de quem está fora desta camada. NUNCA para ler
 * nem escrever diretamente: com TLS, os bytes do socket são cifrados. */
int io_fd(const struct conn_io *io);

/* ---- As operações do chat ---------------------------------------------- */

/* >0 bytes, 0 o peer fechou, -1 erro (errno), IO_AGAIN nada por agora.
 *
 * IO_AGAIN só acontece com TLS: depois do handshake o socket fica
 * NÃO-BLOQUEANTE, para que uma leitura nunca segure o lock da ligação à
 * espera do resto de um record. Quem chama volta ao io_wait_readable(). */
#define IO_AGAIN (-2)
ssize_t io_recv(struct conn_io *io, void *buf, size_t cap);

/* Envia os len bytes todos.
 *   timeout_ms < 0   espera o que for preciso — é o send_all() de sempre.
 *                    Com TLS espera COM o lock de quem chama: o cliente
 *                    usa o io_send_all_shared() (abaixo)
 *   timeout_ms >= 0  prazo TOTAL para a linha inteira (o SERVIDOR, que não
 *                    pode ficar preso por um cliente que não lê)
 *
 *   0                     enviado tudo
 *  -1                     erro do socket: o peer já foi
 *  SEND_TIMEOUT_EXPIRED   prazo esgotado: o peer não está a ler
 *
 * ATENÇÃO (typical-problems.md §1.3): com -1 ou com o prazo esgotado pode já
 * ter saído PARTE da linha, e o stream do peer fica corrompido sem remédio.
 * Quem chama TEM de deixar de escrever nesse socket e fechar a ligação. */
int io_send_all(struct conn_io *io, const void *buf, size_t len, int timeout_ms);

/* Como io_send_all(io, buf, len, -1) — sem prazo —, para quem PARTILHA o SSL*
 * com uma thread que está a ler (o cliente). `ssl_lock` é tomado à volta de
 * cada SSL_write e LARGADO enquanto se espera por espaço no buffer, para que
 * a leitura continue entretanto (M6.md §4, bug 7). Quem chama NÃO o tem.
 *
 * Não impede outra linha de se meter a meio desta: isso é com quem chama
 * (outro lock, à volta da linha inteira). Em claro `ssl_lock` não é usado.
 * 0 = enviado tudo; -1 = erro (o peer já foi). */
int io_send_all_shared(struct conn_io *io, const void *buf, size_t len,
                       pthread_mutex_t *ssl_lock);

/* Esperar por bytes com prazo, acordando se o servidor encerrar (stop_fd).
 * Devolve NET_READY, NET_TIMEOUT, NET_STOP ou -1, como o wait_readable().
 *
 * Com TLS fará mais do que um poll(): o SSL pode já ter bytes decifrados em
 * buffer sem que o socket tenha nada para ler (SSL_pending) — ver M6.md
 * §3.1. É por isso que esta função existe em vez de os chamadores usarem o
 * wait_readable() diretamente com o io_fd(). */
int io_wait_readable(struct conn_io *io, int stop_fd, long timeout_ms);

/* ---- Fecho -------------------------------------------------------------- */

/* Fechar só o sentido de envio: o nosso FIN (e, com TLS, antes dele o
 * close_notify) segue depois do que já está no buffer. É o Ctrl+D do cliente.
 * Com TLS escreve no SSL*: chamar com o lock da ligação. */
void io_shutdown_wr(struct conn_io *io);

/* Fechar os dois sentidos: acorda a thread dona, que está no recv(). É o que
 * o servidor usa quando um envio esgota o prazo. */
void io_shutdown_rw(struct conn_io *io);

/* O fecho do lado de quem fecha: io_shutdown_wr() (close_notify + FIN, logo a
 * seguir à última resposta) e depois esvaziar o que o peer ainda mandou, para
 * que o close() saia como FIN e não como RST (que faria o peer deitar fora a
 * última resposta — M0 nota 4). Como o linger_drain() do net.c.
 *
 * Com TLS escreve no SSL*: ninguém mais lhe pode mexer enquanto corre.
 * Devolve 0 (o peer fechou), 1 (prazo) ou -1. NÃO fecha o fd. */
int io_linger_drain(struct conn_io *io, long timeout_ms);

/* Fecha o fd (se >= 0) e liberta o que a camada tenha. Idempotente: deixa o
 * io com fd = -1. */
void io_close(struct conn_io *io);

#endif /* TLSIO_H */
