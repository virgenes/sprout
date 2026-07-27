#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sprout/log/log.h>

static FILE *g_log_fp = NULL;
static const char *kLevelStr[] = {"INFO", "WARN", "ERROR"};

void log_init(int to_file) {
    if (to_file) {
        g_log_fp = fopen("sprout.log", "a");
        if (g_log_fp) setbuf(g_log_fp, NULL);
    }
    log_info("--- session start ---");
}

void log_shutdown(void) {
    log_info("--- session end ---");
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
}

void log_write(int level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", lt);

    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const char *lvl = (level >= 0 && level < 3) ? kLevelStr[level] : "?";
    fprintf(stdout, "[%s][%s] %s\n", ts, lvl, buf);
    if (g_log_fp) fprintf(g_log_fp, "[%s][%s] %s\n", ts, lvl, buf);
}
