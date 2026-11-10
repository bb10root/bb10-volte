#include "log_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

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
