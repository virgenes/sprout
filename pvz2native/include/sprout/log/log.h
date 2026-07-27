#ifndef SPROUT_LOG_LOG_H
#define SPROUT_LOG_LOG_H

enum { LOG_INFO, LOG_WARN, LOG_ERROR };

void log_init(int to_file);
void log_shutdown(void);
void log_write(int level, const char *fmt, ...);

#define log_info(...)  log_write(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __VA_ARGS__)

#endif
