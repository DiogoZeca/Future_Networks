/*
 * roster.c — STUBS. A implementação vem a seguir.
 *
 * Existem para que tests/unit/test_roster.c LIGUE e corra a vermelho: em C um
 * teste sem implementação nem sequer linka, e um `make test` que não compila é
 * indistinguível de um Makefile partido. Foi o mesmo padrão do framing no M1.
 *
 * Contrato e regras de concorrência: include/roster.h.
 */
#include "roster.h"

void roster_init(struct roster *r)
{
    (void)r;
    /* TODO M2: pthread_mutex_init(&r->lock, NULL); n = 0; */
}

void roster_destroy(struct roster *r)
{
    (void)r;
    /* TODO M2 */
}

struct client *roster_add(struct roster *r, int fd, const char *who)
{
    (void)r; (void)fd; (void)who;
    return NULL;  /* TODO M2 */
}

void roster_remove(struct roster *r, struct client *c)
{
    (void)r; (void)c;
    /* TODO M2 */
}

void client_release(struct roster *r, struct client *c)
{
    (void)r; (void)c;
    /* TODO M2 */
}

size_t roster_snapshot(struct roster *r, struct client **out, size_t cap,
                       const struct client *except)
{
    (void)r; (void)out; (void)cap; (void)except;
    return 0;  /* TODO M2 */
}

size_t roster_count(struct roster *r)
{
    (void)r;
    return 0;  /* TODO M2 */
}
