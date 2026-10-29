#ifndef SYS_UTILS_H
#define SYS_UTILS_H

#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/slog2.h>

#define RUN_PATH "/var/tmp/bb10-volte"

#ifndef LOG_FILE
#define LOG_FILE "/var/log/installer.log"
#endif

extern char g_init_path[512];
extern char g_data_path[512];
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
void init_addon_paths(void);

/* Process & system control */
int run_cmd_spawn(char *const argv[], const char *log_path);
int find_pid_by_name(const char *name, int strict);
int slay_by_cmdline_graceful(const char *search_str, int timeout_ms);

/* File I/O & directory management */
int read_file(const char *path, char *buf, size_t max_len);
int write_file(const char *path, const char *content, mode_t mode);
int append_file(const char *path, const char *content);
int mkdir_p(const char *path, mode_t mode);

#endif /* SYS_UTILS_H */