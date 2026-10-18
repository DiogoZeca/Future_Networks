/*
 * log.c — o log do servidor. Contrato e porquê em include/log.h.
 */
#include "log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define LOG_LINE_MAX 1024              /* o resto de uma linha maior é cortado */

static _Atomic int level = LV_INFO;    /* lido por todas as threads */

static const char *const names[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

void log_set_level(enum log_level lv)
{
    atomic_store(&level, (int)lv);
}

int log_enabled(enum log_level lv)
{
    return (int)lv <= atomic_load(&level);
}

size_t log_format(char *out, size_t cap, enum log_level lv,
                  const struct timespec *when, const char *msg)
{
    struct tm tm;
    time_t    secs = when->tv_sec;
    localtime_r(&secs, &tm);           /* _r: localtime() não é thread-safe */

    int n = snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec,
                     when->tv_nsec / 1000000L,
                     names[lv <= LV_DEBUG ? lv : LV_DEBUG]);
    size_t pos = (n < 0) ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);

    /* A mensagem, sem '\n' nem outros controlos (uma linha é UMA linha). */
    const char *p = msg;
    for (; *p != '\0' && pos + 5 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        out[pos++] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
    }
    if (*p != '\0') {                  /* não coube: dizê-lo */
        memcpy(out + pos, "...", 3);
        pos += 3;
    }
    out[pos++] = '\n';
    out[pos]   = '\0';
    return pos;
}

void log_msg(enum log_level lv, const char *fmt, ...)
{
    if (!log_enabled(lv))
        return;

    char    msg[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    char   line[LOG_LINE_MAX + 64];
    size_t len = log_format(line, sizeof line, lv, &now, msg);

    /* Um só fwrite, com o stderr trancado: uma linha inteira ou nada. */
    flockfile(stderr);
    fwrite(line, 1, len, stderr);
    funlockfile(stderr);
}
