#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void emitir(FILE *out, const char *nivel, const char *fstr, va_list args)
{
    char msg[1024];
    char linea[1100];
    char timestamp[32];

    vsnprintf(msg, sizeof msg, fstr, args);

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", &tm);

    snprintf(linea, sizeof linea, "%s [%s] %s\n", timestamp, nivel, msg);
    fputs(linea, out);
    fflush(out);
}

void log_info(const char *fstr, ...)
{
    va_list args;
    va_start(args, fstr);
    emitir(stdout, "INFO", fstr, args);
    va_end(args);
}

void log_error(const char *fstr, ...)
{
    va_list args;
    va_start(args, fstr);
    emitir(stderr, "ERROR", fstr, args);
    va_end(args);
}