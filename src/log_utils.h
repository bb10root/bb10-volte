#ifndef LOG_UTILS_H
#define LOG_UTILS_H

#include <sys/slog2.h>

#ifndef LOG_FILE
#define LOG_FILE "/var/log/installer.log"
#endif

extern int g_verbose;

/* Global slog2 buffer handle */
extern slog2_buffer_t g_slog_buffer;

#define LOG_VERBOSE(fmt, ...) \
    do { \
        if (g_verbose) { \
            printf("[BBE]:DEBUG: " fmt "\n", ##__VA_ARGS__); \
            fflush(stdout); \
        } \
    } while (0)

/* General utility functions */
void init_slog2_logging(const char *app_name);
void log_msg(const char *fmt, ...);
void bbe_exit_fail(const char *err_msg, int code);

#endif /* LOG_UTILS_H */
