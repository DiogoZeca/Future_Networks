/*
 * framing.c — STUBS. A implementação vem no commit seguinte.
 *
 * Este ficheiro existe para que tests/unit/test_framing.c LIGUE e possa correr
 * a vermelho. Em C, um teste sem a implementação nem sequer linka
 * ("undefined reference"), e um `make test` que não compila é indistinguível de
 * um Makefile partido. Com stubs, o vermelho é um relatório legível — e é essa
 * a prova de que os testes foram escritos antes do código.
 *
 * Invariantes que a implementação terá de manter (ver framing.h):
 *   I1. rx->len <= RX_CAP
 *   I2. rx->scanned <= rx->len
 *   I3. não existe '\n' em data[0..scanned)
 *   I4. rx->failed != 0  =>  rx->len == 0 && rx->scanned == 0
 *   I5. RX_LINE  =>  out[*out_len] == '\0' && 1 <= *out_len <= RX_MAX_LINE-1
 */
#include "framing.h"

void rxbuf_init(struct rxbuf *rx)
{
    (void)rx;
    /* TODO M1: len = 0; scanned = 0; failed = 0; */
}

int rxbuf_feed(struct rxbuf *rx, const char *b, size_t n)
{
    (void)rx; (void)b; (void)n;
    return -1;  /* TODO M1 */
}

int rxbuf_next_line(struct rxbuf *rx, char *out, size_t cap, size_t *out_len)
{
    (void)rx; (void)out; (void)cap; (void)out_len;
    return RX_FAULT;  /* TODO M1 */
}
