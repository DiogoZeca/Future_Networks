/*
 * test_log.c — o formato das linhas de log (include/log.h). M4.
 *
 * Numeração: 89 (a seguir ao 88 da correção do M3).
 */
#include "log.h"
#include "test_util.h"

#include <stdlib.h>

static void caso_89_formato(void)
{
    char            out[128];
    struct timespec t = { .tv_sec = 0, .tv_nsec = 123456789 };

    setenv("TZ", "UTC", 1);              /* a hora não pode depender da máquina */
    tzset();

    CASE(89, "data e hora com ms, nivel com 5 letras, a mensagem, '\\n'");
    size_t n = log_format(out, sizeof out, LV_INFO, &t, "ana saiu");
    CHECK_STR(out, "1970-01-01 00:00:00.123 INFO  ana saiu\n");
    CHECK_INT(n, strlen(out));
    log_format(out, sizeof out, LV_WARN, &t, "x");
    CHECK_STR(out, "1970-01-01 00:00:00.123 WARN  x\n");

    CASE(89, "uma linha e UMA linha: '\\n' e controlos da mensagem viram '?'");
    log_format(out, sizeof out, LV_DEBUG, &t, "a\nb\033c");
    CHECK_STR(out, "1970-01-01 00:00:00.123 DEBUG a?b?c\n");

    CASE(89, "o que nao cabe e cortado com '...', sem escrever alem de cap");
    char small[64];
    memset(small, 'Z', sizeof small);
    char longa[200];
    memset(longa, 'm', sizeof longa - 1);
    longa[sizeof longa - 1] = '\0';
    n = log_format(small, sizeof small, LV_ERROR, &t, longa);
    CHECK(n < sizeof small);
    CHECK_INT(small[n], '\0');
    CHECK_STR(small + n - 4, "...\n");

    CASE(89, "niveis: so passa o que for <= ao nivel escolhido");
    log_set_level(LV_INFO);
    CHECK(log_enabled(LV_ERROR) && log_enabled(LV_WARN) && log_enabled(LV_INFO));
    CHECK(!log_enabled(LV_DEBUG));
    log_set_level(LV_WARN);
    CHECK(!log_enabled(LV_INFO));
    log_set_level(LV_DEBUG);
    CHECK(log_enabled(LV_DEBUG));
}

int main(void)
{
    caso_89_formato();
    return t_report("log");
}
