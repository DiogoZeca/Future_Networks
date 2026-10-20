/*
 * tlsio.c — a E/S de uma ligação, com ou sem TLS. Contrato em include/tlsio.h.
 *
 * Em claro, cada função delega na de net.c que o chamador usava diretamente
 * antes do M6: foi assim que a indirecção entrou sem mudar comportamento
 * nenhum (passo 1 do M6.md).
 *
 * Com TLS, o que muda não é só "passar pelo SSL_write em vez do send":
 *
 *   - SSL_read/SSL_write podem devolver WANT_READ/WANT_WRITE e pedir para
 *     serem chamados outra vez. Em modo bloqueante quase nunca acontece, mas
 *     acontece: numa renegociação, um SSL_read pode precisar de ESCREVER.
 *     É por isso que os ciclos aqui não assumem a direção.
 *
 *   - O TLS lê RECORDS inteiros. Um record pode trazer várias das nossas
 *     linhas: o SSL_read devolve o que lhe cabe no buffer e guarda o resto
 *     DENTRO do SSL*. O socket fica sem nada, o poll() diz "nada", e quem só
 *     olhasse para o poll() adormecia com mensagens já decifradas à espera.
 *     É o mesmo erro do `if` em vez de `while` do M1, uma camada abaixo —
 *     ver io_wait_readable().
 *
 * ESTADO: passo 3. A protecção de concorrência é o passo 5.
 */
#include "tlsio.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* SSL_read e SSL_write recebem um `int`. As nossas linhas são muito menores
 * (RX_CHUNK), mas limitar aqui evita que um size_t grande vire um int
 * negativo no cast — e um tamanho negativo seria aceite em silêncio. */
#define SSL_CHUNK_MAX (1 << 20)

/* ---- utilitários -------------------------------------------------------- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int set_nonblock(int fd, int on)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

/* A primeira linha do erro do OpenSSL, para o log. A fila é POR THREAD, por
 * isso não há corrida aqui — ao contrário do cJSON_GetErrorPtr(), que é um
 * global estático (typical-problems, nota do M1). */
static const char *ssl_err(void)
{
    static _Thread_local char buf[160];
    unsigned long e = ERR_get_error();
    if (e == 0)
        return "sem detalhe do OpenSSL";
    ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return buf;
}

/* ---- em claro ----------------------------------------------------------- */

void io_init_plain(struct conn_io *io, int fd)
{
    io->fd  = fd;
    io->ssl = NULL;
}

int io_is_tls(const struct conn_io *io)
{
    return io->ssl != NULL;
}

int io_fd(const struct conn_io *io)
{
    return io->fd;
}

/* ---- contextos ---------------------------------------------------------- */

void *tls_server_ctx(const char *cert, const char *key, const char **why)
{
    const char *dummy;
    if (why == NULL)
        why = &dummy;

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == NULL) {
        *why = "SSL_CTX_new falhou";
        return NULL;
    }
    /* TLS 1.2 no mínimo: 1.0 e 1.1 estão depreciados (RFC 8996). O OpenSSL 3
     * escolhe 1.3 quando os dois lados o suportam. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
        *why = "nao foi possivel ler o certificado";
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
        *why = "nao foi possivel ler a chave privada";
        SSL_CTX_free(ctx);
        return NULL;
    }
    /* Que a chave é mesmo a deste certificado. Sem isto, um par trocado só
     * daria erro no primeiro handshake — já com o servidor a correr. */
    if (SSL_CTX_check_private_key(ctx) != 1) {
        *why = "a chave privada nao corresponde ao certificado";
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* PARTIAL_WRITE: sem isto, um SSL_write que devolva WANT_WRITE tem de ser
     * repetido com o MESMO ponteiro e o MESMO tamanho — o que colide com o
     * nosso ciclo de envio, que avança o ponteiro (M6.md §4.3b).
     * ACCEPT_MOVING_WRITE_BUFFER: o buffer pode estar noutro sítio na segunda
     * chamada (o nosso avança dentro da mesma linha). */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return ctx;
}

void *tls_client_ctx(const char **why)
{
    const char *dummy;
    if (why == NULL)
        why = &dummy;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        *why = "SSL_CTX_new falhou";
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* SSL_VERIFY_NONE de propósito: o certificado é AUTO-ASSINADO, não há
     * cadeia nenhuma para validar e qualquer verificação falharia sempre.
     * Quem autentica o servidor é o fingerprint, verificado à mão depois do
     * handshake (M6.md §D-TLS-2). Sem essa verificação isto seria cifra sem
     * autenticação — um atacante no meio apresentava o certificado dele. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return ctx;
}

void tls_ctx_free(void *ctx)
{
    if (ctx != NULL)
        SSL_CTX_free((SSL_CTX *)ctx);
}

void io_peer_fingerprint(const struct conn_io *io, char *out, size_t cap)
{
    if (cap == 0)
        return;
    out[0] = '\0';
    if (io->ssl == NULL)
        return;

    X509 *cert = SSL_get1_peer_certificate((SSL *)io->ssl);
    if (cert == NULL)
        return;

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  n = 0;
    if (X509_digest(cert, EVP_sha256(), md, &n) == 1) {
        size_t pos = 0;
        for (unsigned int i = 0; i < n && pos + 3 < cap; i++)
            pos += (size_t)snprintf(out + pos, cap - pos, "%s%02X",
                                    i ? ":" : "", md[i]);
    }
    X509_free(cert);
}

/* ---- handshake ---------------------------------------------------------- */

/* Espera que o fd fique pronto no sentido que o OpenSSL pediu.
 * 0 = pronto, -1 = prazo esgotado ou erro. */
static int wait_for(int fd, int want_write, long deadline)
{
    long left = deadline - now_ms();
    if (left <= 0)
        return -1;
    struct pollfd pf = { .fd = fd, .events = want_write ? POLLOUT : POLLIN };
    int r = poll(&pf, 1, (int)left);
    return r > 0 ? 0 : -1;
}

int io_tls_accept(struct conn_io *io, int fd, void *ctx, int timeout_ms,
                  const char **why)
{
    const char *dummy;
    if (why == NULL)
        why = &dummy;

    io_init_plain(io, fd);

    SSL *ssl = SSL_new((SSL_CTX *)ctx);
    if (ssl == NULL) {
        *why = "SSL_new falhou";
        return -1;
    }
    SSL_set_fd(ssl, fd);

    /* Não-bloqueante só durante o handshake: é o que lhe dá prazo. Um cliente
     * que abre a ligação e nunca fala não pode prender a thread para sempre —
     * e é exatamente o que um varrimento de portas faz. */
    if (set_nonblock(fd, 1) < 0) {
        *why = "fcntl falhou";
        SSL_free(ssl);
        return -1;
    }
    long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    int rc = -1;
    for (;;) {
        int r = SSL_accept(ssl);
        if (r == 1) {                       /* handshake feito */
            rc = 0;
            break;
        }
        int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (wait_for(fd, e == SSL_ERROR_WANT_WRITE, deadline) < 0) {
                *why = "handshake TLS sem resposta (prazo)";
                break;
            }
            continue;
        }
        /* Aqui cai o caso mais comum na prática: alguém ligou-se à porta TLS
         * e mandou JSON em claro (um `nc`, ou um cliente de outro grupo). */
        *why = ssl_err();
        break;
    }

    set_nonblock(fd, 0);                    /* volta a bloqueante */

    if (rc != 0) {
        SSL_free(ssl);
        return -1;
    }
    io->ssl = ssl;
    return 0;
}


/* ---- fingerprint -------------------------------------------------------- */

const char TLS_WHY_FINGERPRINT[] =
    "o certificado do servidor NAO corresponde ao fingerprint";

/* Valor de um dígito hex, ou -1. */
static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Lê um fingerprint em hex (com ou sem ':', maiúsculas ou minúsculas) para
 * out[32]. Devolve 0, ou -1 se não tiver exatamente 64 dígitos hex. */
static int fp_parse(const char *fp, unsigned char out[32])
{
    size_t n = 0;
    int    hi = -1;

    for (const char *p = fp; *p != '\0'; p++) {
        if (*p == ':' || *p == ' ')
            continue;                       /* separadores: ignorados */
        int v = hexval((unsigned char)*p);
        if (v < 0 || n >= 32)
            return -1;                      /* lixo, ou grande de mais */
        if (hi < 0) {
            hi = v;
        } else {
            out[n++] = (unsigned char)((hi << 4) | v);
            hi = -1;
        }
    }
    return (n == 32 && hi < 0) ? 0 : -1;    /* 32 bytes certos, sem meio byte */
}

int tls_valid_fingerprint(const char *fp)
{
    unsigned char raw[32];
    return fp != NULL && fp_parse(fp, raw) == 0;
}

/* O certificado do peer bate certo com `fp`? 0 = sim. */
static int fp_matches(SSL *ssl, const char *fp)
{
    unsigned char want[32];
    if (fp_parse(fp, want) != 0)
        return -1;

    X509 *cert = SSL_get1_peer_certificate(ssl);
    if (cert == NULL)
        return -1;                          /* servidor sem certificado */

    unsigned char got[EVP_MAX_MD_SIZE];
    unsigned int  n = 0;
    int ok = X509_digest(cert, EVP_sha256(), got, &n) == 1 && n == 32 &&
             CRYPTO_memcmp(got, want, 32) == 0;
    /* CRYPTO_memcmp e não memcmp: compara em tempo constante. Aqui não muda
     * nada na prática (o atacante já sabe o certificado que apresentou), mas
     * é o hábito certo para comparar material criptográfico. */
    X509_free(cert);
    return ok ? 0 : -1;
}

int io_tls_connect(struct conn_io *io, int fd, void *ctx, const char *fp,
                   int timeout_ms, const char **why)
{
    const char *dummy;
    if (why == NULL)
        why = &dummy;

    io_init_plain(io, fd);

    SSL *ssl = SSL_new((SSL_CTX *)ctx);
    if (ssl == NULL) {
        *why = "SSL_new falhou";
        return -1;
    }
    SSL_set_fd(ssl, fd);

    if (set_nonblock(fd, 1) < 0) {
        *why = "fcntl falhou";
        SSL_free(ssl);
        return -1;
    }
    long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    int rc = -1;
    for (;;) {
        int r = SSL_connect(ssl);
        if (r == 1) {
            rc = 0;
            break;
        }
        int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (wait_for(fd, e == SSL_ERROR_WANT_WRITE, deadline) < 0) {
                *why = "handshake TLS sem resposta (prazo)";
                break;
            }
            continue;
        }
        /* O caso mais provável: ligámos a uma porta que não fala TLS. */
        *why = ssl_err();
        break;
    }

    /* O HANDSHAKE TER CORRIDO BEM NÃO CHEGA. Até aqui está cifrado, mas não
     * se sabe com QUEM: o certificado é auto-assinado e qualquer um pode
     * gerar um. É esta verificação que transforma "cifrado" em "cifrado com
     * o servidor certo" — e é ela que trava um man-in-the-middle. */
    if (rc == 0 && fp != NULL && fp_matches(ssl, fp) != 0) {
        *why = TLS_WHY_FINGERPRINT;
        rc = -1;
    }

    if (rc != 0) {
        set_nonblock(fd, 0);
        SSL_free(ssl);
        return -1;
    }
    io->ssl = ssl;          /* fica não-bloqueante: ver io_tls_accept */
    return 0;
}

/* ---- E/S ---------------------------------------------------------------- */


ssize_t io_recv(struct conn_io *io, void *buf, size_t cap)
{
    if (io->ssl == NULL)
        return recv_retry(io->fd, buf, cap);

    SSL *ssl = (SSL *)io->ssl;
    for (;;) {
        int n = SSL_read(ssl, buf, (int)(cap > SSL_CHUNK_MAX ? SSL_CHUNK_MAX : cap));
        if (n > 0)
            return n;

        int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_ZERO_RETURN)     /* close_notify: o FIN do TLS */
            return 0;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            return IO_AGAIN;                /* não há record completo: esperar */
        if (e == SSL_ERROR_SYSCALL && n == 0)
            return 0;                       /* o peer fechou sem close_notify */
        return -1;
    }
}

int io_send_all(struct conn_io *io, const void *buf, size_t len, int timeout_ms)
{
    if (io->ssl == NULL) {
        /* Sem prazo é o caso do cliente: só escreve para o servidor, e
         * esperar é melhor do que desistir. Com prazo é o do servidor, que
         * não pode ficar preso por um cliente que não lê (M4). */
        if (timeout_ms < 0)
            return send_all(io->fd, buf, len);
        return send_all_timeout(io->fd, buf, len, timeout_ms);
    }

    SSL        *ssl      = (SSL *)io->ssl;
    const char *p        = buf;
    long        deadline = timeout_ms >= 0 ? now_ms() + timeout_ms : 0;

    while (len > 0) {
        int n = SSL_write(ssl, p, (int)(len > SSL_CHUNK_MAX ? SSL_CHUNK_MAX : len));
        if (n > 0) {
            p   += n;
            len -= (size_t)n;
            continue;
        }
        int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            /* O socket é não-bloqueante (depois do handshake), por isso um
             * WANT_* aqui é "o buffer encheu": esperar, nunca repetir em
             * ciclo — isso seria queimar CPU. */
            struct pollfd pf = { .fd = io->fd,
                                 .events = (e == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN };
            int left = -1;                  /* sem prazo: o cliente */
            if (timeout_ms >= 0) {
                long l = deadline - now_ms();
                if (l <= 0)
                    return SEND_TIMEOUT_EXPIRED;
                left = (int)l;
            }
            if (poll(&pf, 1, left) <= 0)
                return timeout_ms >= 0 ? SEND_TIMEOUT_EXPIRED : -1;
            continue;
        }
        return -1;
    }
    return 0;
}

int io_wait_readable(struct conn_io *io, int stop_fd, long timeout_ms)
{
    /* O PASSO QUE NÃO PODE FALTAR: com TLS pode haver bytes JÁ DECIFRADOS no
     * buffer do SSL sem que o socket tenha nada para ler. O poll() diria
     * "nada" e o ciclo adormecia com mensagens à espera — o mesmo erro do
     * `if` em vez de `while` do M1, uma camada abaixo (M6.md §4.3a). */
    if (io->ssl != NULL && SSL_pending((SSL *)io->ssl) > 0)
        return NET_READY;

    return wait_readable(io->fd, stop_fd, timeout_ms);
}

/* ---- fecho -------------------------------------------------------------- */

void io_shutdown_wr(struct conn_io *io)
{
    if (io->fd < 0)
        return;
    /* Com TLS, o "fim de dados" é o close_notify, não o FIN. Mandamo-lo e
     * fechamos o nosso lado de envio na mesma: o peer vê os dois. Não se
     * espera pelo close_notify DELE — isso é com o io_linger_drain. */
    if (io->ssl != NULL)
        SSL_shutdown((SSL *)io->ssl);
    shutdown(io->fd, SHUT_WR);
}

void io_shutdown_rw(struct conn_io *io)
{
    if (io->fd >= 0)
        shutdown(io->fd, SHUT_RDWR);        /* acorda a thread que está no recv */
}

int io_linger_drain(struct conn_io *io, long timeout_ms)
{
    if (io->ssl == NULL)
        return linger_drain(io->fd, timeout_ms);

    /* Com TLS os bytes do socket são records cifrados: lê-los em bruto não
     * diz nada. Esvazia-se pelo SSL_read, que também trata o close_notify. */
    long  deadline = now_ms() + timeout_ms;
    char  scrap[1024];
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0)
            return 1;
        if (SSL_pending((SSL *)io->ssl) == 0 &&
            wait_readable(io->fd, -1, left) != NET_READY)
            return 1;
        ssize_t n = io_recv(io, scrap, sizeof scrap);
        if (n == 0)
            return 0;                       /* o peer fechou */
        if (n == IO_AGAIN)
            continue;                       /* record incompleto: esperar mais */
        if (n < 0)
            return -1;
    }
}

void io_close(struct conn_io *io)
{
    if (io->ssl != NULL) {
        SSL_free((SSL *)io->ssl);           /* não fecha o fd: SSL_set_fd não é dono */
        io->ssl = NULL;
    }
    if (io->fd >= 0)
        close(io->fd);
    io->fd = -1;
}
