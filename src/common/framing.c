/*
 * framing.c — bytes <-> linhas. Contrato e decisões em include/framing.h e
 * docs/framing.md; aqui só o "como".
 *
 * Invariantes mantidos por todas as funções:
 *   I1. rx->len <= RX_CAP
 *   I2. rx->scanned <= rx->len
 *   I3. não existe '\n' em data[0..scanned)
 *   I4. rx->failed != 0  =>  rx->len == 0 && rx->scanned == 0
 *   I5. RX_LINE  =>  out[*out_len] == '\0' && 1 <= *out_len <= RX_MAX_LINE-1
 */
#include "framing.h"

#include <assert.h>
#include <string.h>

/* I1, I2 e I4 verificados à entrada de cada função. Custam zero em release
 * (-DNDEBUG); nos testes transformam um bug de índice num abort com a linha
 * exata, em vez de dados corrompidos mais tarde.
 * I3 fica de fora de propósito: verificá-lo é um memchr sobre data[0..scanned),
 * ou seja, O(n) por chamada — anulava a razão de ser do `scanned`. */
static void check_invariants(const struct rxbuf *rx)
{
    assert(rx->len <= RX_CAP);                                   /* I1 */
    assert(rx->scanned <= rx->len);                              /* I2 */
    assert(!rx->failed || (rx->len == 0 && rx->scanned == 0));   /* I4 */
    (void)rx;   /* evita aviso de não-usado quando NDEBUG apaga os asserts */
}

/* Envenena o buffer: deita fora tudo e recusa trabalho até ao próximo init.
 * Esvaziar não é só higiene — é o que torna o I4 verdade. */
static void poison(struct rxbuf *rx)
{
    rx->len     = 0;
    rx->scanned = 0;
    rx->failed  = 1;
}

void rxbuf_init(struct rxbuf *rx)
{
    rx->len      = 0;
    rx->scanned  = 0;
    rx->failed   = 0;
    rx->lenient  = 0;
    rx->skipping = 0;
    /* data[] não precisa de ser limpo: só data[0..len) é alguma vez lido. */
}

void rxbuf_init_lenient(struct rxbuf *rx)
{
    rxbuf_init(rx);
    rx->lenient = 1;
}

/* Modo tolerante: deita fora data[0..n) e recomeça a procura. */
static void drop_front(struct rxbuf *rx, size_t n)
{
    rx->len -= n;
    memmove(rx->data, rx->data + n, rx->len);
    rx->scanned = 0;
}

int rxbuf_feed(struct rxbuf *rx, const char *b, size_t n)
{
    check_invariants(rx);

    /* Envenenado recusa tudo, incluindo n == 0: "só o init limpa" é mais
     * fácil de raciocinar sem exceções. */
    if (rx->failed)
        return -1;

    if (n == 0)
        return 0;

    /* Escrito como subtração, não como `rx->len + n > RX_CAP`: a soma pode dar
     * a volta (overflow de size_t) com um n absurdo; a subtração não, porque
     * o I1 garante len <= RX_CAP. */
    if (n > RX_CAP - rx->len) {
        poison(rx);
        return -1;
    }

    memcpy(rx->data + rx->len, b, n);
    rx->len += n;
    /* `scanned` NÃO muda: os bytes novos ficam depois dele, ainda por procurar. */
    return 0;
}

int rxbuf_next_line(struct rxbuf *rx, char *out, size_t cap, size_t *out_len)
{
    check_invariants(rx);

    /* 1. Envenenado: a resposta é sempre a mesma (idempotente, teste #10). */
    if (rx->failed)
        return RX_TOOLONG;

    /* 2. Erro do CHAMADOR, não do peer. Vem depois do 1 porque um buffer
     *    envenenado é a notícia mais importante. */
    if (out == NULL || cap < RX_LINE_BUF)
        return RX_FAULT;

    /* O ciclo só existe para saltar linhas vazias (passo 9) e, no modo
     * tolerante, o fim de uma linha longa que se está a descartar. */
    for (;;) {
        /* 3. Procurar '\n' só nos bytes AINDA NÃO examinados. */
        char *nl = memchr(rx->data + rx->scanned, '\n', rx->len - rx->scanned);

        /* 3b. Modo tolerante, a meio de uma linha longa: tudo até ao '\n' (e
         *     ele próprio) é dessa linha e vai fora. Sem '\n', vai tudo fora. */
        if (rx->skipping) {
            if (nl == NULL) {
                rx->len = rx->scanned = 0;
                return RX_NEED;
            }
            drop_front(rx, (size_t)(nl - rx->data) + 1);
            rx->skipping = 0;
            continue;
        }

        /* 4. Não há linha completa. */
        if (nl == NULL) {
            rx->scanned = rx->len;              /* tudo examinado, sem '\n' (I3) */
            /* `>=` e não `>`: com len == RX_MAX_LINE e sem '\n', qualquer linha
             * que acabe aqui terá pelo menos RX_MAX_LINE + 1 bytes. Detetar já
             * é o que mantém o resíduo <= RX_MAX_LINE - 1 (framing.md §3.1). */
            if (rx->len >= RX_MAX_LINE) {
                if (rx->lenient) {              /* descartar até ao '\n' */
                    rx->len = rx->scanned = 0;
                    rx->skipping = 1;
                    return RX_SKIPPED;
                }
                poison(rx);
                return RX_TOOLONG;
            }
            return RX_NEED;
        }

        /* 5. Bytes a consumir do buffer, INCLUINDO o '\n'. */
        size_t consume = (size_t)(nl - rx->data) + 1;
        if (consume > RX_MAX_LINE) {            /* RX_MAX_LINE cabe, +1 não (#21) */
            if (rx->lenient) {                  /* a linha acaba aqui: tirá-la */
                drop_front(rx, consume);        /* e guardar o que vem a seguir */
                return RX_SKIPPED;
            }
            poison(rx);
            return RX_TOOLONG;
        }

        /* 6. Conteúdo = sem o '\n', e sem UM '\r' final se existir (#5). */
        size_t content = consume - 1;
        if (content > 0 && rx->data[content - 1] == '\r')
            content--;

        /* 7. Copiar para fora PRIMEIRO... */
        memcpy(out, rx->data, content);         /* content <= RX_MAX_LINE-1 < cap */
        out[content] = '\0';

        /* 8. ...e só DEPOIS compactar. Ao contrário, out receberia os bytes da
         *    mensagem seguinte — e sem crash nenhum. memmove e não memcpy:
         *    origem e destino sobrepõem-se, memcpy aí é comportamento indefinido. */
        rx->len -= consume;
        memmove(rx->data, rx->data + consume, rx->len);
        rx->scanned = 0;                        /* tudo mudou de sítio */

        /* 9. Linha vazia ("\n" ou "\r\n" isolado): saltar, nunca devolver (#8). */
        if (content == 0)
            continue;

        /* 10. Sucesso. */
        if (out_len != NULL)
            *out_len = content;
        return RX_LINE;
    }
}
