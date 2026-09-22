// util.c -- implementation of the logging helpers.

#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX 1024

// Advance past a *printf() result, clamping so we never run past the buffer.
static size_t bump(size_t off, int ret, size_t cap)
{
    if (ret < 0)
        return off;
    if ((size_t)ret >= cap - off)
        return cap - 1;
    return off + (size_t)ret;
}

static void vlog(int fd, int with_errno, const char *fmt, va_list ap)
{
    int         saved_errno = errno;
    char        line[LOG_LINE_MAX];
    char        stamp[32];
    struct tm   tmv;
    time_t      now = time(NULL);
    size_t      off = 0;
    ssize_t     ignored;

    if (localtime_r(&now, &tmv) != NULL)
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    else
        snprintf(stamp, sizeof(stamp), "no timestamp");

    off = bump(off, snprintf(line, sizeof(line), "[%s] ", stamp), sizeof(line));

    if (off < sizeof(line) - 1)
        off = bump(off, vsnprintf(line + off, sizeof(line) - off, fmt, ap),
                   sizeof(line));

    if (with_errno && off < sizeof(line) - 1)
        off = bump(off, snprintf(line + off, sizeof(line) - off, ": %s",
                                 strerror(saved_errno)),
                   sizeof(line));

    line[off++] = '\n';

    ignored = write(fd, line, off);
    (void)ignored;

    errno = saved_errno;
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDOUT_FILENO, 0, fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 1, fmt, ap);
    va_end(ap);
}

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 0, fmt, ap);
    va_end(ap);
}

void die_sys(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 1, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

void die_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(STDERR_FILENO, 0, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}
