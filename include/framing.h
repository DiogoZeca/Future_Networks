/*
 * framing.h — bytes <-> linhas.
 *
 * TCP é um stream de bytes: não preserva as fronteiras dos send(). Medimos no
 * M0 que um send() único de 10 001 bytes chegou ao servidor em TRÊS recv()
 * (4096 + 4096 + 1809). O inverso também acontece: duas mensagens rápidas
 * chegam coladas num só recv().
 *
 * Esta camada resolve isso: acumula bytes e entrega só LINHAS COMPLETAS,
 * delimitadas por '\n' (PROTOCOL.md §2). Não sabe o que é JSON — isso é do
 * protocol.h. É essa separação que a torna testável sem rede.
 *
 * Uso típico:
 *
 *     rxbuf_init(&rx);
 *     for (;;) {
 *         n = recv(fd, chunk, RX_CHUNK);
 *         if (n <= 0) break;
 *         if (rxbuf_feed(&rx, chunk, n) != 0) { ...linha demasiado longa... }
 *         for (;;) {
 *             int r = rxbuf_next_line(&rx, line, sizeof line, &len);
 *             if (r == RX_NEED) break;        // faltam bytes: voltar ao recv()
 *             if (r != RX_LINE) { ...fechar... }
 *             processar(line, len);
 *         }
 *     }
 *
 * O ciclo interno tem de ser um `for`/`while`, NUNCA um `if`: um recv() pode
 * produzir zero, uma, ou muitas mensagens. Com `if`, a segunda mensagem de um
 * par colado fica esquecida no buffer — passa em todos os testes manuais e
 * falha quando duas pessoas escrevem ao mesmo tempo.
 *
 * E tem de ramificar em TRÊS: comparar com `== RX_LINE` torna RX_TOOLONG
 * indistinguível de RX_NEED, e volta-se ao recv() com o buffer envenenado.
 */
#ifndef FRAMING_H
#define FRAMING_H

#include <stddef.h>

/* ---- Capacidades -------------------------------------------------------- */

/* Comprimento máximo de uma linha, INCLUINDO o '\n' (PROTOCOL.md §2). */
#define RX_MAX_LINE 4096

/* Tamanho mínimo do out[] passado a rxbuf_next_line(): o conteúdo sem o '\n'
 * (<= RX_MAX_LINE - 1) mais o '\0' terminador. */
#define RX_LINE_BUF RX_MAX_LINE

/* Máximo que um recv() deve entregar a um único rxbuf_feed(). */
#define RX_CHUNK 4096

/* Capacidade do buffer interno.
 *
 * NÃO é um número redondo escolhido por gosto — é a soma de duas quantidades:
 * depois de drenar todas as linhas completas, o resíduo é uma linha incompleta,
 * logo sem '\n', logo <= RX_MAX_LINE - 1 = 4095 bytes. O feed seguinte traz no
 * máximo RX_CHUNK = 4096. Pior caso: 4095 + 4096 = 8191 <= 8192.
 *
 * O _Static_assert abaixo garante que continua a ser verdade se alguém mexer
 * num dos lados. */
#define RX_CAP (RX_MAX_LINE + RX_CHUNK)

_Static_assert(RX_CAP >= RX_MAX_LINE + RX_CHUNK,
               "RX_CAP tem de conter o resto da linha anterior MAIS um recv() inteiro");
_Static_assert(RX_LINE_BUF >= RX_MAX_LINE,
               "out[] tem de caber o conteudo da linha mais o '\\0'");

/* ---- Estado ------------------------------------------------------------- */

struct rxbuf {
    size_t len;      /* bytes válidos em data[0..len)                        */
    size_t scanned;  /* data[0..scanned) já foi procurado e NÃO contém '\n'  */
    int    failed;   /* 1 = envenenado; só rxbuf_init() limpa                */
    char   data[RX_CAP];
};

/*
 * `scanned` evita varrer o buffer todo a cada chamada. Com feed byte-a-byte
 * (que é um dos testes) isso seria O(n^2): ~8M comparações por linha de 4 KB.
 * Só é escrito em dois sítios: `= len` quando não se encontrou '\n', e `= 0`
 * depois de extrair uma linha (porque o memmove desloca tudo).
 *
 * `failed` torna "nunca ressincronizar" (PROTOCOL.md, decisão D2) uma
 * propriedade ESTRUTURAL em vez de uma convenção que alguém esquece. Depois de
 * uma linha demasiado longa não se sabe onde começa a mensagem seguinte, por
 * isso o buffer fica inutilizável de propósito.
 *
 * A struct é pública (não opaca) porque no M2 vai ser embebida POR VALOR dentro
 * de uma struct conn — sem malloc, logo sem caminho de free para esquecer.
 */

/* ---- Valores de retorno ------------------------------------------------- */

enum {
    RX_LINE    =  1,  /* linha extraída; out[] NUL-terminado; *out_len >= 1   */
    RX_NEED    =  0,  /* faltam bytes: chamar recv() outra vez                */
    RX_TOOLONG = -1,  /* culpa DELES: linha > RX_MAX_LINE. Buffer envenenado  */
    RX_FAULT   = -2   /* culpa NOSSA: out == NULL, ou cap < RX_LINE_BUF       */
};

/* A separação RX_TOOLONG / RX_FAULT é "culpa deles" vs "culpa nossa". Ambos
 * levam a fechar a ligação, mas o log é diferente — e é essa a diferença entre
 * depurar em 2 minutos ou em 2 horas. RX_FAULT nunca deve aparecer em produção;
 * se aparecer num teste, é um bug de chamada, não uma mensagem malformada. */

/* ---- API ---------------------------------------------------------------- */

/* Põe o buffer no estado inicial. É também a ÚNICA forma de limpar o estado de
 * erro — não existe rxbuf_reset() de propósito: ter duas formas de limpar é um
 * convite a usar a errada a seguir a um erro. */
void rxbuf_init(struct rxbuf *rx);

/* Acrescenta n bytes ao buffer.
 *
 * Devolve 0 em sucesso.
 * Devolve -1 se os bytes não couberem (o que só pode acontecer se o chamador
 * violar a disciplina "feed de <= RX_CHUNK, depois drenar tudo") ou se o buffer
 * já estiver envenenado. Em qualquer dos casos ENVENENA o buffer.
 *
 * n == 0 é um no-op com retorno 0, não um erro.
 *
 * `b` pode conter '\0' e bytes que não são UTF-8 válido: são opacos até haver
 * uma linha completa. Partir em '\n' é sempre seguro em UTF-8, porque todos os
 * bytes de continuação de uma sequência multibyte são >= 0x80 e portanto 0x0A
 * nunca ocorre dentro de um caracter. */
int rxbuf_feed(struct rxbuf *rx, const char *b, size_t n);

/* Extrai a próxima linha completa.
 *
 *   out      destino do conteúdo; tem de ter pelo menos RX_LINE_BUF bytes
 *   cap      sizeof(out)
 *   out_len  comprimento do conteúdo, SEM o '\n' (e sem o '\r' de um "\r\n").
 *            Pode ser NULL se não interessar.
 *
 * Devolve RX_LINE, RX_NEED, RX_TOOLONG ou RX_FAULT (ver acima).
 *
 * Com RX_LINE, out[] fica NUL-terminado e *out_len >= 1 — linhas vazias são
 * saltadas aqui dentro, nunca devolvidas ao chamador. Se fossem devolvidas,
 * cada chamador precisaria do mesmo `if (len == 0) continue;`, e o do cliente
 * seria esquecido. De borda, isto trata um "\r\n" isolado como keep-alive, que
 * é o que um peer de outro grupo mais provavelmente há-de mandar.
 *
 * out_len é necessário (e não basta strlen(out)) porque uma linha pode conter
 * um '\0' embebido: sem o comprimento, o resto desapareceria em silêncio. É
 * também o que permite usar cJSON_ParseWithLength() em vez de cJSON_Parse(). */
int rxbuf_next_line(struct rxbuf *rx, char *out, size_t cap, size_t *out_len);

/* ---- Leitura de estado (para logs e testes) ----------------------------- */

static inline size_t rxbuf_pending(const struct rxbuf *rx) { return rx->len; }
static inline int    rxbuf_failed (const struct rxbuf *rx) { return rx->failed; }

#endif /* FRAMING_H */
