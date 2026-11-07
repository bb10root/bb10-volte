#ifndef PROC_UTILS_H
#define PROC_UTILS_H

#include <stddef.h>
#include <sys/types.h>

/* Process & system control */
int run_cmd_spawn(char *const argv[], const char *log_path);
int find_pid_by_name(const char *name, int strict);
int slay_by_cmdline_graceful(const char *search_str, int timeout_ms);

/* Async-signal-safe: writes the process's executable path, or "unknown". size must be at least 8. */
void exe_name_signal_safe(pid_t pid, char *out, size_t size);

#endif /* PROC_UTILS_H */
