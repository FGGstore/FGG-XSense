#include "log.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static FILE *g_log;

int log_open(const char *dir, const char *path)
{
    mkdir(dir, 0755);
    g_log = fopen(path, "ab");
    return g_log != NULL;
}

void log_close(void)
{
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

void log_line(const char *fmt, ...)
{
    char stamp[32];
    time_t now = time(NULL);
    struct tm tmv;
    va_list ap;

    if (!g_log) return;

    gmtime_r(&now, &tmv);
    if (strftime(stamp, sizeof stamp, "%H:%M:%S", &tmv) == 0)
        strcpy(stamp, "00:00:00");

    va_start(ap, fmt);
    fprintf(g_log, "%s ", stamp);
    vfprintf(g_log, fmt, ap);
    fprintf(g_log, "\n");
    va_end(ap);

    /* Flushed per line on purpose: an unflushed buffer is lost if the payload
     * faults, and a fault is exactly when the log matters most. */
    fflush(g_log);
}

void notify(const char *fmt, ...)
{
    notify_request_t req;
    char msg[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
