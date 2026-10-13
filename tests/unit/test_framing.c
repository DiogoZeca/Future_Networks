/*
 * test_framing.c — testes da camada de enquadramento (bytes <-> linhas).
 *
 * Sem rede. É isso que torna possível reproduzir à vontade os casos que em
 * produção acontecem ao acaso: mensagem partida a meio, duas coladas, linha
 * gigante. Com rede, metade destes casos era impossível de provocar de forma
 * determinística.
 *
 * Numeração: casos 1-10 vêm da tabela em docs/milestones/M1.md; 19-22 foram
 * acrescentados na revisão do plano; 23-24 nasceram de testes de mutação
 * (bugs introduzidos de propósito que nenhum dos outros apanhava).
 *
 * Foram escritos ANTES da implementação (commit 70e7fa6, contra stubs): são
 * eles que definem o contrato.
 */
#include "framing.h"
#include "test_util.h"

/* Buffers grandes em static, não na stack. */
static struct rxbuf rx;
static char line[RX_LINE_BUF];
static char big[RX_CAP * 2];

/* ------------------------------------------------------------------------ */

/* 1. Uma mensagem completa, entregue de uma vez. O caminho feliz. */
static void caso_1_de_uma_vez(void)
{
    CASE(1, "mensagem completa num unico feed");
    const char *in = "{\"a\":1}\n";
    size_t len = 0;

    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, in, strlen(in)), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "{\"a\":1}");
    CHECK_INT(len, 7);
    CHECK_INT(rxbuf_pending(&rx), 0);                 /* buffer ficou vazio */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_NEED);
}

/* 2. A MESMA mensagem, entregue byte a byte.
 *    O teste mais valioso de todos: apanha quase todos os bugs de
 *    enquadramento e é trivial de escrever. Só o ultimo byte (o '\n') pode
 *    produzir uma linha. */
static void caso_2_byte_a_byte(void)
{
    CASE(2, "mensagem entregue byte a byte");
    const char *in = "{\"a\":1}\n";
    size_t len = 0;

    rxbuf_init(&rx);
    for (size_t i = 0; in[i] != '\0'; i++) {
        CHECK_INT(rxbuf_feed(&rx, &in[i], 1), 0);
        int r = rxbuf_next_line(&rx, line, sizeof line, &len);
        CHECK_INT(r, in[i] == '\n' ? RX_LINE : RX_NEED);
    }
    CHECK_STR(line, "{\"a\":1}");
    CHECK_INT(rxbuf_pending(&rx), 0);
}

/* 3. Duas mensagens num único feed.
 *    É o caso que obriga o chamador a usar `while` e não `if`. */
static void caso_3_duas_num_feed(void)
{
    CASE(3, "duas mensagens coladas num feed");
    const char *in = "{\"type\":\"NICK\"}\n{\"type\":\"MSG\"}\n";
    size_t len = 0;

    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, in, strlen(in)), 0);

    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "{\"type\":\"NICK\"}");

    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "{\"type\":\"MSG\"}");

    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_NEED);
    CHECK_INT(rxbuf_pending(&rx), 0);
}

/* 4. Uma mensagem e meia: a segunda fica por completar e TEM de ser preservada. */
static void caso_4_uma_e_meia(void)
{
    CASE(4, "uma mensagem e meia: o resto fica no buffer");
    size_t len = 0;

    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, "msg1\nmsg", 8), 0);

    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "msg1");
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_NEED);
    CHECK_INT(rxbuf_pending(&rx), 3);                 /* "msg" continua la */

    CHECK_INT(rxbuf_feed(&rx, "2\n", 2), 0);          /* completa-se agora */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "msg2");
}

/* 5. Terminação "\r\n": o '\r' é descartado (PROTOCOL.md §2 manda aceitar).
 *    Um só '\r' — tirar todos seria inventar semantica que a spec nao tem. */
static void caso_5_crlf(void)
{
    CASE(5, "terminacao CRLF: o '\\r' e descartado");
    size_t len = 0;

    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, "ola\r\n", 5), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "ola");
    CHECK_INT(len, 3);

    CASE(5, "apenas UM '\\r' e removido");
    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, "a\r\r\n", 4), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_INT(len, 2);                                /* fica "a\r" */
}

/* 6. Linha demasiado longa: erro controlado, SEM overflow.
 *    Sob `make SAN=1` isto deixa de ser auto-afirmacao — o AddressSanitizer
 *    confirma que nao houve escrita fora de data[RX_CAP]. */
static void caso_6_linha_gigante(void)
{
    CASE(6, "5000 bytes sem '\\n' -> RX_TOOLONG, sem overflow");
    size_t len = 0;

    rxbuf_init(&rx);
    memset(big, 'A', 5000);
    CHECK_INT(rxbuf_feed(&rx, big, 5000), 0);         /* cabe em RX_CAP */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_TOOLONG);
    CHECK_INT(rxbuf_failed(&rx), 1);                  /* envenenado */
    CHECK_INT(rxbuf_pending(&rx), 0);                 /* e esvaziado */
}

/* 7. Linha exactamente no limite: RX_MAX_LINE inclui o '\n',
 *    logo 4095 de conteudo + '\n' = 4096 e legal. */
static void caso_7_limite_exacto(void)
{
    CASE(7, "linha exactamente no limite e aceite");
    size_t len = 0;

    rxbuf_init(&rx);
    memset(big, 'B', RX_MAX_LINE - 1);
    big[RX_MAX_LINE - 1] = '\n';
    CHECK_INT(rxbuf_feed(&rx, big, RX_MAX_LINE), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_INT(len, RX_MAX_LINE - 1);
    CHECK_INT(rxbuf_failed(&rx), 0);
}

/* 8. Linhas vazias: ignoradas, nao sao erro.
 *    Sao saltadas DENTRO do next_line, por isso o chamador nunca as ve. */
static void caso_8_linhas_vazias(void)
{
    CASE(8, "linhas vazias sao ignoradas");
    size_t len = 0;

    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, "\n\n\nola\n", 7), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "ola");                           /* saltou as tres */
    CHECK_INT(len, 3);

    CASE(8, "\"\\r\\n\" isolado tambem e ignorado (keep-alive)");
    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, "\r\n", 2), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_NEED);
}

/* 9. UTF-8 partido entre dois feed.
 *    "é" = 0xC3 0xA9. Partir em '\n' e sempre seguro porque os bytes de
 *    continuacao sao >= 0x80, logo 0x0A nunca ocorre dentro de um caracter. */
static void caso_9_utf8_partido(void)
{
    CASE(9, "UTF-8 partido entre dois feed");
    size_t len = 0;

    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, "ol\xC3", 3), 0);       /* primeiro byte do "a" */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_NEED);
    CHECK_INT(rxbuf_feed(&rx, "\xA1\n", 2), 0);       /* segundo byte + '\n'  */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "ol\xC3\xA1");                    /* "olá" intacto */
    CHECK_INT(len, 4);                                /* 4 BYTES, 3 caracteres */
}

/* 10. Depois de um erro, o estado e EXACTO e nao ha ressincronizacao.
 *     So o rxbuf_init() limpa. */
static void caso_10_depois_do_erro(void)
{
    CASE(10, "buffer envenenado: feed e next_line recusam, init limpa");
    size_t len = 0;

    rxbuf_init(&rx);
    memset(big, 'C', 5000);
    CHECK_INT(rxbuf_feed(&rx, big, 5000), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_TOOLONG);

    CHECK_INT(rxbuf_failed(&rx), 1);
    CHECK_INT(rxbuf_pending(&rx), 0);
    CHECK_INT(rxbuf_feed(&rx, "ola\n", 4), -1);       /* recusa */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_TOOLONG); /* idempotente */

    rxbuf_init(&rx);                                  /* a unica forma de limpar */
    CHECK_INT(rxbuf_failed(&rx), 0);
    CHECK_INT(rxbuf_feed(&rx, "ola\n", 4), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_STR(line, "ola");
}

/* 19. feed de zero bytes: no-op, nao e erro. */
static void caso_19_feed_vazio(void)
{
    CASE(19, "feed de 0 bytes e no-op, nao erro");
    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, "", 0), 0);
    CHECK_INT(rxbuf_pending(&rx), 0);
    CHECK_INT(rxbuf_failed(&rx), 0);

    CHECK_INT(rxbuf_feed(&rx, "ab", 2), 0);
    CHECK_INT(rxbuf_feed(&rx, "", 0), 0);             /* nao destroi o que la esta */
    CHECK_INT(rxbuf_pending(&rx), 2);
}

/* 20. '\0' embebido numa linha.
 *     E isto que justifica o out_len: com strlen(out) o resto desaparecia
 *     em silencio. */
static void caso_20_nul_embebido(void)
{
    CASE(20, "'\\0' embebido: out_len != strlen(out)");
    const char in[] = { '{','"','a','"',':','"','x','\0','y','"','}','\n' };
    size_t len = 0;

    rxbuf_init(&rx);
    CHECK_INT(rxbuf_feed(&rx, in, sizeof in), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_INT(len, 11);                               /* conteudo sem o '\n' */
    CHECK_INT(strlen(line), 7);                       /* strlen para no '\0' */
    CHECK_MEM(line, in, 11);                          /* mas os bytes la estao */
}

/* 21. A fronteira, dos DOIS lados. */
static void caso_21_fronteira(void)
{
    CASE(21, "4095+'\\n' aceite; 4096+'\\n' rejeitado");
    size_t len = 0;

    rxbuf_init(&rx);
    memset(big, 'D', RX_MAX_LINE - 1);
    big[RX_MAX_LINE - 1] = '\n';
    CHECK_INT(rxbuf_feed(&rx, big, RX_MAX_LINE), 0);          /* 4096 total */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);

    rxbuf_init(&rx);
    memset(big, 'E', RX_MAX_LINE);
    big[RX_MAX_LINE] = '\n';
    CHECK_INT(rxbuf_feed(&rx, big, RX_MAX_LINE + 1), 0);      /* 4097 total */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_TOOLONG);
}

/* 22. O PIOR CASO que justifica RX_CAP = RX_MAX_LINE + RX_CHUNK.
 *     Residuo maximo (4095, sem '\n') mais um recv() inteiro (4096) = 8191.
 *     Este teste executa a aritmetica do D1 em vez de a argumentar — vale
 *     tanto como os outros todos juntos para a defesa. */
static void caso_22_pior_caso_capacidade(void)
{
    CASE(22, "residuo 4095 + chunk 4096 = 8191 cabe em RX_CAP");
    size_t len = 0;

    rxbuf_init(&rx);
    memset(big, 'F', RX_MAX_LINE - 1);
    CHECK_INT(rxbuf_feed(&rx, big, RX_MAX_LINE - 1), 0);      /* 4095, sem '\n' */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_NEED);
    CHECK_INT(rxbuf_pending(&rx), RX_MAX_LINE - 1);
    CHECK_INT(rxbuf_failed(&rx), 0);

    memset(big, 'G', RX_CHUNK);
    big[0] = '\n';                                            /* fecha a linha */
    CHECK_INT(rxbuf_feed(&rx, big, RX_CHUNK), 0);             /* 4095+4096=8191 */
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_LINE);
    CHECK_INT(len, RX_MAX_LINE - 1);
    CHECK_INT(rxbuf_pending(&rx), RX_CHUNK - 1);              /* o resto fica */
}

/* 23. Exactamente RX_MAX_LINE bytes e ainda sem '\n': ja e demasiado longa.
 *     Qualquer '\n' que chegue a seguir daria uma linha de >= 4097 bytes, por
 *     isso nao ha razao para esperar. Apanha o bug `>` em vez de `>=` no
 *     passo 4, que nenhum outro teste apanhava: com `>`, um peer que mande
 *     4096 bytes e pare fica a ser esperado em vez de ser recusado. */
static void caso_23_limite_sem_newline(void)
{
    CASE(23, "4096 bytes sem '\\n' -> RX_TOOLONG logo, sem esperar");
    size_t len = 0;

    rxbuf_init(&rx);
    memset(big, 'H', RX_MAX_LINE);
    CHECK_INT(rxbuf_feed(&rx, big, RX_MAX_LINE), 0);
    CHECK_INT(rxbuf_next_line(&rx, line, sizeof line, &len), RX_TOOLONG);
    CHECK_INT(rxbuf_failed(&rx), 1);
}

/* 24. Duas mensagens byte a byte, SEM init entre elas.
 *     O #2 acaba logo depois da primeira linha, por isso nunca exercita o
 *     estado a seguir a uma extracao com scanned > 0. Este sim: apanha o bug
 *     de esquecer `scanned = 0` depois do memmove (dispara o assert do I2; sem
 *     asserts, o memchr recebe um comprimento "negativo" e o ASan apanha). */
static void caso_24_duas_byte_a_byte(void)
{
    CASE(24, "duas mensagens byte a byte, sem init entre elas");
    const char *in = "abc\nde\n";
    size_t len = 0;
    int linhas = 0;

    rxbuf_init(&rx);
    for (size_t i = 0; in[i] != '\0'; i++) {
        CHECK_INT(rxbuf_feed(&rx, &in[i], 1), 0);
        int r = rxbuf_next_line(&rx, line, sizeof line, &len);
        CHECK_INT(r, in[i] == '\n' ? RX_LINE : RX_NEED);
        if (r == RX_LINE)
            linhas++;
    }
    CHECK_INT(linhas, 2);
    CHECK_STR(line, "de");
    CHECK_INT(rxbuf_pending(&rx), 0);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    caso_1_de_uma_vez();
    caso_2_byte_a_byte();
    caso_3_duas_num_feed();
    caso_4_uma_e_meia();
    caso_5_crlf();
    caso_6_linha_gigante();
    caso_7_limite_exacto();
    caso_8_linhas_vazias();
    caso_9_utf8_partido();
    caso_10_depois_do_erro();
    caso_19_feed_vazio();
    caso_20_nul_embebido();
    caso_21_fronteira();
    caso_22_pior_caso_capacidade();
    caso_23_limite_sem_newline();
    caso_24_duas_byte_a_byte();
    return t_report("framing");
}
