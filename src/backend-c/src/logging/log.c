#include "magitrickle/log.h"
#include "magitrickle/log_history.h"
#include "magitrickle/rand.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MT_LOG_STACK_SIZE 1024

static pthread_mutex_t g_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_level = MT_LOG_INFO;
static _Atomic int g_fd = STDOUT_FILENO;

#include "log_history.inc"

void mt_log_set_level(mt_log_level_t level)
{
    atomic_store_explicit(&g_level, (int)level, memory_order_relaxed);
}

mt_log_level_t mt_log_level(void)
{
    return (mt_log_level_t)atomic_load_explicit(&g_level, memory_order_relaxed);
}

void mt_log_set_fd(int fd)
{
    atomic_store_explicit(&g_fd, fd, memory_order_relaxed);
}

mt_log_level_t mt_log_level_from_str(const char *s)
{
    if (s == NULL) return MT_LOG_INFO;
    static const struct {
        const char *name;
        mt_log_level_t level;
    } levels[] = {
        {"trace", MT_LOG_TRACE}, {"debug", MT_LOG_DEBUG},
        {"info", MT_LOG_INFO}, {"warn", MT_LOG_WARN},
        {"error", MT_LOG_ERROR}, {"fatal", MT_LOG_FATAL},
        {"panic", MT_LOG_PANIC}, {"nolevel", MT_LOG_NOLEVEL},
        {"disabled", MT_LOG_DISABLED},
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(s, levels[i].name) == 0) return levels[i].level;
    }
    return MT_LOG_INFO;
}

static const char *level_tag(mt_log_level_t level)
{
    switch (level) {
    case MT_LOG_TRACE: return "TRC";
    case MT_LOG_DEBUG: return "DBG";
    case MT_LOG_INFO: return "INF";
    case MT_LOG_WARN: return "WRN";
    case MT_LOG_ERROR: return "ERR";
    case MT_LOG_FATAL: return "FTL";
    case MT_LOG_PANIC: return "PNC";
    case MT_LOG_NOLEVEL: return "???";
    case MT_LOG_DISABLED: return "OFF";
    }
    return "???";
}

/* Capture does not depend on the output sink being writable. Neither the
 * history mutex nor any network operation is held across console writes. */
static void write_log_line(mt_log_level_t level, const char *line, size_t len)
{
    history_append(level, line, len);
    const int fd = atomic_load_explicit(&g_fd, memory_order_relaxed);
    (void)pthread_mutex_lock(&g_write_mutex);
    while (len > 0) {
        size_t amount = len > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : len;
        ssize_t written = write(fd, line, amount);
        if (written > 0) {
            line += (size_t)written;
            len -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break; /* No busy-loop on a failed/nonblocking sink. */
        }
    }
    (void)pthread_mutex_unlock(&g_write_mutex);
}

static void log_format_failure(mt_log_level_t level, const char *prefix,
                               size_t prefix_len, const char *reason)
{
    char line[128];
    int len = snprintf(line, sizeof(line), "%.*s[log message omitted: %s]\n",
                       (int)prefix_len, prefix, reason);
    if (len > 0 && (size_t)len < sizeof(line)) {
        write_log_line(level, line, (size_t)len);
    }
}

void mt_log(mt_log_level_t level, const char *fmt, ...)
{
    if ((int)level < atomic_load_explicit(&g_level, memory_order_relaxed) ||
        level >= MT_LOG_DISABLED) return;

    const int saved_errno = errno;
    char stack[MT_LOG_STACK_SIZE];
    struct timespec ts;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || !gmtime_r(&ts.tv_sec, &tm)) {
        errno = saved_errno;
        return;
    }
    int off = snprintf(stack, sizeof(stack),
                       "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %s ",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
                       level_tag(level));
    if (off < 0 || (size_t)off >= sizeof(stack)) {
        errno = saved_errno;
        return;
    }
    const size_t prefix_len = (size_t)off;
    va_list ap;
    va_start(ap, fmt);
    errno = saved_errno;
    int needed = vsnprintf(stack + prefix_len, sizeof(stack) - prefix_len, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        log_format_failure(level, stack, prefix_len, "formatting failed");
    } else if ((size_t)needed < sizeof(stack) - prefix_len) {
        size_t len = prefix_len + (size_t)needed;
        stack[len++] = '\n';
        write_log_line(level, stack, len);
    } else if ((size_t)needed > SIZE_MAX - prefix_len - 2) {
        log_format_failure(level, stack, prefix_len, "length overflow");
    } else {
        const size_t len = prefix_len + (size_t)needed + 1;
        char *line = malloc(len + 1);
        if (!line) {
            log_format_failure(level, stack, prefix_len, "out of memory");
        } else {
            memcpy(line, stack, prefix_len);
            va_start(ap, fmt);
            errno = saved_errno;
            int formatted = vsnprintf(line + prefix_len, (size_t)needed + 1, fmt, ap);
            va_end(ap);
            if (formatted == needed) {
                line[len - 1] = '\n';
                write_log_line(level, line, len);
            } else {
                log_format_failure(level, stack, prefix_len, "formatting changed");
            }
            free(line);
        }
    }
    errno = saved_errno;
}
