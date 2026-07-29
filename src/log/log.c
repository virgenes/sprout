#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sprout/log/log.h>

static FILE *g_log_fp = NULL;

static const char *kChannelNames[LOG_CHAN_COUNT] = {
    "DEX", "JIT", "GFX", "AUDIO", "INPUT", "ENGINE", "VFS", "IAP", "VERSION",
};

static const char *kLevelStr[] = {"DEBUG", "INFO", "WARN", "ERROR"};

static int g_channel_enabled[LOG_CHAN_COUNT];
static int g_initialized = 0;

void log_init(int to_file) {
    for (int i = 0; i < LOG_CHAN_COUNT; i++)
        g_channel_enabled[i] = 1;
    g_initialized = 1;

    if (to_file) {
        g_log_fp = fopen("sprout.log", "a");
        if (g_log_fp) setbuf(g_log_fp, NULL);
    }
    log_info("--- session start ---");
}

void log_shutdown(void) {
    log_info("--- session end ---");
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
    g_initialized = 0;
}

void log_set_channel_enabled(int channel, int enabled) {
    if (channel >= 0 && channel < LOG_CHAN_COUNT)
        g_channel_enabled[channel] = enabled ? 1 : 0;
}

int log_get_channel_enabled(int channel) {
    return (channel >= 0 && channel < LOG_CHAN_COUNT) ? g_channel_enabled[channel] : 1;
}

void log_write(int channel, int level, const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", lt);

    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    const char *lvl = (level >= 0 && level < 4) ? kLevelStr[level] : "?";
    if (channel >= 0 && channel < LOG_CHAN_COUNT) {
        fprintf(stdout, "[%s][%s][%s] %s\n", ts, kChannelNames[channel], lvl, buf);
        if (g_log_fp) fprintf(g_log_fp, "[%s][%s][%s] %s\n", ts, kChannelNames[channel], lvl, buf);
    } else {
        fprintf(stdout, "[%s][%s] %s\n", ts, lvl, buf);
        if (g_log_fp) fprintf(g_log_fp, "[%s][%s] %s\n", ts, lvl, buf);
    }
}
