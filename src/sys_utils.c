#include "sys_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <spawn.h>
#include <dirent.h>
#include <libgen.h>
#include <limits.h>
#include <sys/wait.h>

char g_init_path[512] = {0};
char g_data_path[512] = {0};
int g_verbose = 0;

/* Global slog2 buffer handle */
slog2_buffer_t g_slog_buffer = NULL;

/* Initialize QNX slog2 buffer registration */
void init_slog2_logging(const char *app_name)
{
    slog2_buffer_set_config_t buffer_config;
    memset(&buffer_config, 0, sizeof(buffer_config));

    buffer_config.buffer_set_name = (char *)app_name;
    buffer_config.num_buffers = 1;
    buffer_config.verbosity_level = SLOG2_DEBUG1;

    /* Set 4KB ring buffer for application logging */
    buffer_config.buffer_config[0].buffer_name = "default";
    buffer_config.buffer_config[0].num_pages = 1;

    if (slog2_register(&buffer_config, &g_slog_buffer, 0) != 0)
    {
        fprintf(stderr, "[BBE] Warning: Failed to register slog2 buffer\n");
    }
}

void log_msg(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[32];
    if (t)
    {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
    }
    else
    {
        snprintf(time_str, sizeof(time_str), "0000-00-00 00:00:00");
    }

    char buffer[1024];
    va_list args;

    /* 1. Send to slog2 system logger if initialized */
    if (g_slog_buffer != NULL)
    {
        va_start(args, fmt);
        vslog2f(g_slog_buffer, 0, SLOG2_INFO, fmt, args);
        va_end(args);
    }

    /* 2. Format string for stdout and log file */
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    printf("%s %s\n", time_str, buffer);

    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0)
    {
        char file_line[1100];
        int len = snprintf(file_line, sizeof(file_line), "%s %s\n", time_str, buffer);
        if (len > 0)
        {
            write(fd, file_line, (size_t)len);
        }
        close(fd);
    }
}

void bbe_exit_fail(const char *err_msg, int code)
{
    log_msg("[BBE] ERROR: %s", err_msg);
    exit(code);
}

/* Low-level POSIX I/O utilities using O_CLOEXEC */

int read_file(const char *path, char *buf, size_t max_len)
{
    if (!path || !buf || max_len == 0)
        return -1;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;

    ssize_t r = read(fd, buf, max_len - 1);
    close(fd);

    if (r <= 0)
        return -1;

    buf[r] = '\0';
    return (int)r;
}

int write_file(const char *path, const char *content, mode_t mode)
{
    if (!path || !content)
        return -1;

    mode_t open_mode = (mode != 0) ? mode : 0644;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, open_mode);
    if (fd < 0)
        return -1;

    size_t len = strlen(content);
    ssize_t written = 0;

    while ((size_t)written < len)
    {
        ssize_t res = write(fd, content + written, len - written);
        if (res < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        written += res;
    }

    close(fd);
    return 0;
}

int append_file(const char *path, const char *content)
{
    if (!path || !content)
        return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;

    size_t len = strlen(content);
    ssize_t written = 0;

    while ((size_t)written < len)
    {
        ssize_t res = write(fd, content + written, len - written);
        if (res < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        written += res;
    }

    close(fd);
    return 0;
}

int mkdir_p(const char *path, mode_t mode)
{
    if (!path || *path == '\0')
        return -1;

    char tmp[PATH_MAX];
    int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len < 0 || (size_t)len >= sizeof(tmp))
        return -1;

    /* Strip trailing slashes */
    while (len > 1 && tmp[len - 1] == '/')
    {
        tmp[len - 1] = '\0';
        len--;
    }

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

/* Process execution and control helpers */

int run_cmd_spawn(char *const argv[], const char *log_path)
{
    if (!argv || !argv[0])
        return -1;

    int fd = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (log_path)
    {
        fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0)
        {
            posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
            posix_spawn_file_actions_addclose(&actions, fd);
        }
    }

    pid_t pid;
    int status = posix_spawnp(&pid, argv[0], &actions, NULL, argv, NULL);

    posix_spawn_file_actions_destroy(&actions);
    if (fd >= 0)
        close(fd);

    if (status != 0)
        return -1;

    if (waitpid(pid, &status, 0) == -1)
        return -1;

    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int find_pid_by_name(const char *name, int strict)
{
    if (!name)
        return -1;

    DIR *d = opendir("/proc");
    if (!d)
        return -1;

    struct dirent *ent;
    int found_pid = -1;

    while ((ent = readdir(d)))
    {
        int pid = atoi(ent->d_name);
        if (pid <= 0)
            continue;

        char path[PATH_MAX];
        char exe_path[PATH_MAX];

        snprintf(path, sizeof(path), "/proc/%d/exefile", pid);

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            ssize_t n = read(fd, exe_path, sizeof(exe_path) - 1);
            close(fd);

            if (n > 0)
            {
                exe_path[n] = '\0';

                char *newline = strchr(exe_path, '\n');
                if (newline)
                    *newline = '\0';

                if (strict)
                {
                    if (strcmp(exe_path, name) == 0)
                    {
                        found_pid = pid;
                        break;
                    }
                }
                else
                {
                    if (strstr(exe_path, name) != NULL)
                    {
                        found_pid = pid;
                        break;
                    }
                }
            }
        }
    }
    closedir(d);
    return found_pid;
}

int slay_by_cmdline_graceful(const char *search_str, int timeout_ms)
{
    if (!search_str || strlen(search_str) == 0)
        return -1;

    DIR *d = opendir("/proc");
    if (!d)
        return -1;

    struct dirent *ent;
    size_t capacity = 16;
    size_t pid_count = 0;
    pid_t *pids = malloc(capacity * sizeof(pid_t));
    if (!pids)
    {
        closedir(d);
        log_msg("Failed to allocate memory for PIDs");
        return -1;
    }

    pid_t self_pid = getpid();

    /* Step 1: Scan /proc and collect matching process IDs */
    while ((ent = readdir(d)))
    {
        int pid = atoi(ent->d_name);
        if (pid <= 0 || pid == (int)self_pid)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            char cmdline[2048];
            ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
            close(fd);

            if (n > 0)
            {
                for (ssize_t i = 0; i < n - 1; i++)
                {
                    if (cmdline[i] == '\0')
                        cmdline[i] = ' ';
                }
                cmdline[n] = '\0';

                if (strstr(cmdline, search_str) != NULL)
                {
                    if (pid_count >= capacity)
                    {
                        size_t new_capacity = capacity * 2;
                        pid_t *new_pids = realloc(pids, new_capacity * sizeof(pid_t));
                        if (!new_pids)
                        {
                            log_msg("Memory reallocation failed, processing partial list (%zu processes)", pid_count);
                            break;
                        }
                        pids = new_pids;
                        capacity = new_capacity;
                    }
                    pids[pid_count++] = (pid_t)pid;
                }
            }
        }
    }
    closedir(d);

    if (pid_count == 0)
    {
        free(pids);
        log_msg("No active processes found matching cmdline pattern: %s", search_str);
        return -1;
    }

    /* Step 2: Send SIGTERM signal to all matched processes */
    int active_pids = 0;
    for (size_t i = 0; i < pid_count; i++)
    {
        if (kill(pids[i], SIGTERM) == 0)
        {
            log_msg("Signal SIGTERM sent to PID %d", pids[i]);
            active_pids++;
        }
        else
        {
            log_msg("Failed to send SIGTERM to PID %d: %s", pids[i], strerror(errno));
            pids[i] = -1;
        }
    }

    if (active_pids == 0)
    {
        free(pids);
        return 0;
    }

    /* Step 3: Poll process exit state until timeout */
    int step_us = 50000; /* 50 ms */
    int elapsed_us = 0;
    int timeout_us = timeout_ms * 1000;

    while (elapsed_us < timeout_us)
    {
        usleep(step_us);
        elapsed_us += step_us;

        int still_running = 0;
        for (size_t i = 0; i < pid_count; i++)
        {
            if (pids[i] <= 0)
                continue;

            if (kill(pids[i], 0) == 0)
            {
                still_running++;
            }
            else
            {
                pids[i] = -1;
            }
        }

        if (still_running == 0)
            break;
    }

    /* Step 4: Send SIGKILL to non-responsive processes */
    for (size_t i = 0; i < pid_count; i++)
    {
        if (pids[i] > 0 && kill(pids[i], 0) == 0)
        {
            if (kill(pids[i], SIGKILL) == 0)
            {
                log_msg("Process PID %d did not exit after %d ms; sent SIGKILL", pids[i], timeout_ms);
            }
            else
            {
                log_msg("Failed to send SIGKILL to PID %d: %s", pids[i], strerror(errno));
            }
        }
    }

    free(pids);
    return 0;
}

void init_addon_paths(void)
{
    char exe_path[512];

    if (read_file("/proc/self/exefile", exe_path, sizeof(exe_path)) > 0)
    {
        exe_path[strcspn(exe_path, "\r\n")] = '\0';

        char *init_dir = dirname(exe_path);
        snprintf(g_init_path, sizeof(g_init_path), "%s", init_dir);

        char init_dir_copy[512];
        snprintf(init_dir_copy, sizeof(init_dir_copy), "%s", init_dir);
        char *base_dir = dirname(init_dir_copy);
        snprintf(g_data_path, sizeof(g_data_path), "%s/data", base_dir);
    }
    else
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
        {
            snprintf(g_init_path, sizeof(g_init_path), "%s", cwd);
            char *parent = dirname(cwd);
            snprintf(g_data_path, sizeof(g_data_path), "%s/data", parent);
        }
        else
        {
            snprintf(g_init_path, sizeof(g_init_path), "./init");
            snprintf(g_data_path, sizeof(g_data_path), "./data");
        }
    }
}