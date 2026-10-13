/*
 * test_util.h — runner mínimo para os testes unitários.
 *
 * Porquê não um framework (Unity, Check, CUnit, greatest): são casos sobre
 * funções puras, sem rede, sem mocks, sem fixtures, sem setup/teardown. Tudo o
 * que é preciso é "chamar uma função, comparar um valor, contar" — e isso são
 * estas ~50 linhas. O Check e o CUnit exigiriam instalação no sistema, ou seja
 * mais uma barreira para quem clonar o repositório.
 *
 * O trabalho pesado (overflows, uso de memória não inicializada, fugas) é do
 * `make SAN=1`, não deste ficheiro. Aqui só é preciso contar e devolver o
 * código de saída certo.
 *
 * Incluído por UM .c por binário de teste — daí os `static`, que assim não
 * colidem nem dão avisos de não-usado.
 */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int         t_pass = 0;
static int         t_fail = 0;
static const char *t_case = "(sem caso)";

/* Identifica o caso a que pertencem os CHECK seguintes. O número refere-se à
 * tabela de testes em docs/milestones/M1.md. */
#define CASE(n, desc) do { t_case = "#" #n " " desc; } while (0)

#define CHECK(cond) do {                                                       \
    if (cond) { t_pass++; }                                                    \
    else { t_fail++;                                                           \
        printf("  FALHA  %s\n         %s:%d  %s\n",                            \
               t_case, __FILE__, __LINE__, #cond); }                           \
} while (0)

#define CHECK_INT(got, want) do {                                              \
    long g_ = (long)(got), w_ = (long)(want);                                  \
    if (g_ == w_) { t_pass++; }                                                \
    else { t_fail++;                                                           \
        printf("  FALHA  %s\n         %s:%d  %s: obtido %ld, esperado %ld\n",  \
               t_case, __FILE__, __LINE__, #got, g_, w_); }                    \
} while (0)

#define CHECK_STR(got, want) do {                                              \
    const char *g_ = (got), *w_ = (want);                                      \
    if (g_ != NULL && strcmp(g_, w_) == 0) { t_pass++; }                       \
    else { t_fail++;                                                           \
        printf("  FALHA  %s\n         %s:%d  obtido \"%s\", esperado \"%s\"\n",\
               t_case, __FILE__, __LINE__, g_ ? g_ : "(null)", w_); }          \
} while (0)

/* Compara n bytes — para conteúdo com '\0' embebido, onde CHECK_STR mentiria. */
#define CHECK_MEM(got, want, n) do {                                           \
    if (memcmp((got), (want), (n)) == 0) { t_pass++; }                         \
    else { t_fail++;                                                           \
        printf("  FALHA  %s\n         %s:%d  %zu bytes diferem\n",             \
               t_case, __FILE__, __LINE__, (size_t)(n)); }                     \
} while (0)

/* Imprime o resumo e devolve o que o main() deve devolver: 0 se tudo passou,
 * 1 caso contrário — é isto que faz o `make test` falhar de facto. */
static int t_report(const char *suite)
{
    printf("%s: %d ok, %d falhas\n", suite, t_pass, t_fail);
    return t_fail == 0 ? 0 : 1;
}

#endif /* TEST_UTIL_H */
