/*
 * roster.c — a lista de clientes partilhada. Contrato e regras de concorrência
 * em include/roster.h; aqui só o "como".
 *
 * Duas regras governam este ficheiro:
 *
 *   1. Tudo o que lê ou escreve r->v[], r->n, c->refs ou c->listed fá-lo com
 *      r->lock agarrado. Sem exceções.
 *   2. destroy_client() é chamado SEMPRE fora do lock (exceto em
 *      roster_destroy, que é o fim do programa). close() e free() não precisam
 *      do lock, e segurá-lo durante um close() seria dar a um cliente a
 *      possibilidade de atrasar toda a gente.
 *
 * O padrão repete-se em roster_remove() e client_release():
 *
 *      lock; decrementar; anotar se chegou a zero; unlock;
 *      se chegou a zero, destruir.
 *
 * É isso que mantém a regra 2 verdadeira sem perder a atomicidade do
 * decremento.
 */
#include "roster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Fecha o fd, destrói o lock de escrita e liberta. Só deve ser chamado quando
 * refs chegou a 0 — ou seja, quando mais ninguém tem o ponteiro. */
static void destroy_client(struct client *c)
{
    if (c->fd >= 0)
        close(c->fd);               /* a última referência fecha o socket */
    pthread_mutex_destroy(&c->wlock);
    free(c);
}

void roster_init(struct roster *r)
{
    pthread_mutex_init(&r->lock, NULL);
    r->n = 0;
}

void roster_destroy(struct roster *r)
{
    /* Liberta tudo, IGNORANDO o refcount: é o fim do programa (ou de um teste)
     * e já não há threads a correr. Se houvesse, isto era use-after-free — o
     * contrato avisa. */
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < r->n; i++) {
        r->v[i]->listed = 0;
        destroy_client(r->v[i]);
    }
    r->n = 0;
    pthread_mutex_unlock(&r->lock);
    pthread_mutex_destroy(&r->lock);
}

struct client *roster_add(struct roster *r, int fd, const char *who)
{
    /* Alocar e preparar FORA do lock: calloc() e pthread_mutex_init() não
     * precisam dele, e segurá-lo durante uma alocação atrasava toda a gente. */
    struct client *c = calloc(1, sizeof *c);
    if (c == NULL)
        return NULL;

    c->fd = fd;
    snprintf(c->who, sizeof c->who, "%s", who);
    c->nick[0] = '\0';              /* vazio até o NICK ser aceite */
    atomic_init(&c->dead, 0);       /* ninguém lhe escreve antes do add */

    if (pthread_mutex_init(&c->wlock, NULL) != 0) {
        free(c);
        return NULL;
    }

    pthread_mutex_lock(&r->lock);
    if (r->n >= ROSTER_MAX) {
        pthread_mutex_unlock(&r->lock);
        pthread_mutex_destroy(&c->wlock);
        free(c);
        return NULL;                /* lista cheia: o chamador recusa a ligação */
    }
    /* Duas referências: a da lista e a de quem chamou (a thread dona). */
    c->refs   = 2;
    c->listed = 1;
    r->v[r->n++] = c;
    pthread_mutex_unlock(&r->lock);

    return c;
}

void roster_remove(struct roster *r, struct client *c)
{
    if (c == NULL)
        return;

    int destroy = 0;

    pthread_mutex_lock(&r->lock);
    /* `listed` é o que torna a segunda chamada inofensiva: sem ele, remover
     * duas vezes largava duas referências e dava um free a mais. */
    if (c->listed) {
        for (size_t i = 0; i < r->n; i++) {
            if (r->v[i] == c) {
                /* Trocar com o último em vez de deslocar o resto: a ordem da
                 * lista não tem significado nenhum, e assim é O(1). */
                r->v[i] = r->v[r->n - 1];
                r->n--;
                break;
            }
        }
        c->listed = 0;
        if (--c->refs == 0)         /* larga a referência DA LISTA */
            destroy = 1;
    }
    pthread_mutex_unlock(&r->lock);

    if (destroy)                    /* fora do lock (regra 2) */
        destroy_client(c);
}

void roster_join(struct roster *r, struct client *c, const char *nick)
{
    pthread_mutex_lock(&r->lock);
    snprintf(c->nick, sizeof c->nick, "%s", nick);
    c->joined = 1;
    pthread_mutex_unlock(&r->lock);
}

void client_release(struct roster *r, struct client *c)
{
    if (c == NULL)
        return;

    int destroy = 0;

    pthread_mutex_lock(&r->lock);
    if (--c->refs == 0)
        destroy = 1;
    pthread_mutex_unlock(&r->lock);

    if (destroy)
        destroy_client(c);
}

size_t roster_snapshot(struct roster *r, struct client **out, size_t cap,
                       const struct client *except)
{
    size_t n = 0;

    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < r->n && n < cap; i++) {
        if (r->v[i] == except)
            continue;
        /* Quem ainda não fez NICK não está no chat: receberia EVENT e CHAT
         * antes do OK ao seu próprio NICK. */
        if (!r->v[i]->joined)
            continue;
        /* O incremento acontece DENTRO do lock, ao mesmo tempo que a leitura
         * do ponteiro. É esta atomicidade que fecha a janela: a partir daqui o
         * cliente não pode ser libertado antes de quem o leva largar a
         * referência, mesmo que saia da lista entretanto. */
        r->v[i]->refs++;
        out[n++] = r->v[i];
    }
    pthread_mutex_unlock(&r->lock);

    return n;
}

size_t roster_count(struct roster *r)
{
    pthread_mutex_lock(&r->lock);
    size_t n = r->n;
    pthread_mutex_unlock(&r->lock);
    return n;
}

/* ---- Nicks (M3) --------------------------------------------------------- */

/* Quem na lista (que não `self`) tem este nick? Chamar com r->lock. Percorre
 * TODOS, não só os joined: quem reservou e ainda não entrou também conta. */
static struct client *nick_owner_locked(struct roster *r, const char *nick,
                                        const struct client *self, int joined_only)
{
    for (size_t i = 0; i < r->n; i++) {
        struct client *o = r->v[i];
        if (o == self || o->nick[0] == '\0')
            continue;
        if (joined_only && !o->joined)
            continue;
        if (proto_nick_eq(o->nick, nick))
            return o;
    }
    return NULL;
}

int roster_claim(struct roster *r, struct client *c, const char *nick)
{
    int ok = 0;

    pthread_mutex_lock(&r->lock);
    /* A verificação e a escrita NO MESMO lock: é isto que torna os nicks
     * únicos mesmo com dois NICK iguais a chegar no mesmo instante. */
    if (nick_owner_locked(r, nick, c, 0) == NULL) {
        snprintf(c->nick, sizeof c->nick, "%s", nick);
        ok = 1;
    }
    pthread_mutex_unlock(&r->lock);
    return ok;
}

int roster_lookup(struct roster *r, const char *nick, char canon[PROTO_NICK_MAX + 1])
{
    pthread_mutex_lock(&r->lock);
    struct client *o = nick_owner_locked(r, nick, NULL, 1);
    if (o != NULL && canon != NULL)
        memcpy(canon, o->nick, PROTO_NICK_MAX + 1);
    pthread_mutex_unlock(&r->lock);
    return o != NULL;
}

struct client *roster_find(struct roster *r, const char *nick)
{
    pthread_mutex_lock(&r->lock);
    struct client *o = nick_owner_locked(r, nick, NULL, 1);
    if (o != NULL)
        o->refs++;                  /* dentro do lock, como no retrato (D7) */
    pthread_mutex_unlock(&r->lock);
    return o;
}

size_t roster_nicks(struct roster *r, char (*out)[PROTO_NICK_MAX + 1], size_t cap)
{
    size_t n = 0;

    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < r->n && n < cap; i++)
        if (r->v[i]->joined)
            memcpy(out[n++], r->v[i]->nick, PROTO_NICK_MAX + 1);
    pthread_mutex_unlock(&r->lock);
    return n;
}
