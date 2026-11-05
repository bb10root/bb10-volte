#ifndef PROC_UTILS_H
#define PROC_UTILS_H

/* Process & system control */
int run_cmd_spawn(char *const argv[], const char *log_path);
int find_pid_by_name(const char *name, int strict);
int slay_by_cmdline_graceful(const char *search_str, int timeout_ms);

#endif /* PROC_UTILS_H */
