/* Run with `make test`. Builds proc_utils.c without QNX's slog2. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The real header needs QNX's slog2. */
#define LOG_UTILS_H
#define LOG_VERBOSE(...) ((void)0)
void log_msg(const char *fmt, ...);

#include "proc_utils.c"

void log_msg(const char *fmt, ...)
{
    (void)fmt;
}

static void check_path(pid_t pid, const char *expected)
{
    char path[32];

    build_exefile_path(path, pid);
    assert(strcmp(path, expected) == 0);
}

/* Reads a file holding `content` through a buffer of `size` bytes. */
static void read_back(const char *content, size_t size, char *out)
{
    char name[] = "build/stop_sender_XXXXXX";
    int fd = mkstemp(name);

    assert(fd >= 0);
    assert(write(fd, content, strlen(content)) == (ssize_t)strlen(content));
    close(fd);

    read_line_signal_safe(name, out, size);
    unlink(name);
}

int main(void)
{
    char out[256];
    char small[8];

    check_path(0, "/proc/0/exefile");
    check_path(7, "/proc/7/exefile");
    check_path(14774363, "/proc/14774363/exefile");

    read_back("/base/usr/bin/slay\nextra\n", sizeof(out), out);
    assert(strcmp(out, "/base/usr/bin/slay") == 0);

    read_back("/base/usr/bin/slay", sizeof(out), out);
    assert(strcmp(out, "/base/usr/bin/slay") == 0);

    read_back("/base/usr/bin/slay", sizeof(small), small);
    assert(strcmp(small, "/base/u") == 0);

    read_back("", sizeof(out), out);
    assert(strcmp(out, "unknown") == 0);

    read_line_signal_safe("build/stop_sender_missing", out, sizeof(out));
    assert(strcmp(out, "unknown") == 0);

    puts("stop sender tests passed");
    return 0;
}
