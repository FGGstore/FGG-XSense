#include "log.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

/* A log past this size is moved aside and started afresh. */
#define LOG_LIMIT (2L * 1024 * 1024)

static FILE *g_log;
static char g_path[128], g_old_path[128];
static long g_size;

/* Backends log from their own threads; one line must not interleave with
 * another. */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

int log_open(const char *dir, const char *path, const char *old_path)
{
    mkdir(dir, 0755);
    snprintf(g_path, sizeof g_path, "%s", path);
    snprintf(g_old_path, sizeof g_old_path, "%s", old_path ? old_path : "");
    g_log = fopen(path, "ab");
    if (!g_log) return 0;
    fseek(g_log, 0, SEEK_END);
    g_size = ftell(g_log);
    return 1;
}

/* With g_log_lock held. */
static void rotate(void)
{
    if (!g_log || !g_old_path[0]) return;
    fclose(g_log);
    rename(g_path, g_old_path);
    g_log = fopen(g_path, "ab");
    g_size = 0;
}

void log_fresh(void)
{
    pthread_mutex_lock(&g_log_lock);
    rotate();
    pthread_mutex_unlock(&g_log_lock);
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

    pthread_mutex_lock(&g_log_lock);
    gmtime_r(&now, &tmv);
    if (strftime(stamp, sizeof stamp, "%H:%M:%S", &tmv) == 0)
        strcpy(stamp, "00:00:00");

    va_start(ap, fmt);
    g_size += fprintf(g_log, "%s ", stamp);
    g_size += vfprintf(g_log, fmt, ap);
    g_size += fprintf(g_log, "\n");
    va_end(ap);

    /* Flushed per line on purpose: an unflushed buffer is lost if the payload
     * faults, and a fault is exactly when the log matters most. */
    fflush(g_log);
    if (g_size > LOG_LIMIT) rotate();
    pthread_mutex_unlock(&g_log_lock);
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
