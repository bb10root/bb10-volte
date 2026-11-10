/* Run with `make test`. Builds ims_patch.c without QNX's slog2 and the daemon. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The real header needs QNX's slog2. */
#define LOG_UTILS_H
void log_msg(const char *fmt, ...);

#include "ims_patch.c"

char g_init_path[512];
char g_data_path[512];

void log_msg(const char *fmt, ...)
{
    (void)fmt;
}

int run_cmd_spawn(char *const argv[], const char *log_path)
{
    (void)argv;
    (void)log_path;
    return 0;
}

#define REAL_BINARY "re/q10/ims_rrm"
#define SIZE (0x24c702 + 16)
#define COUNT (sizeof(patches) / sizeof(patches[0]))

static void put(unsigned char *buf, size_t i, int to)
{
    memcpy(buf + patches[i].off, to ? patches[i].to : patches[i].from, 2);
}

static void check_buffer(void)
{
    unsigned char *buf = calloc(1, SIZE);
    unsigned char *copy = calloc(1, SIZE);

    assert(buf && copy);

    /* Neither site matches */
    assert(ims_patch_buffer(buf, SIZE) == -1);
    assert(memcmp(buf, copy, SIZE) == 0);

    /* Only one site matches: nothing is written */
    put(buf, 0, 0);
    memcpy(copy, buf, SIZE);
    assert(ims_patch_buffer(buf, SIZE) == -1);
    assert(memcmp(buf, copy, SIZE) == 0);

    /* Too short for the second site */
    put(buf, 1, 0);
    assert(ims_patch_buffer(buf, 0xd9ffa + 1) == -1);

    /* All match */
    for (size_t i = 2; i < COUNT; i++)
        put(buf, i, 0);
    assert(ims_patch_buffer(buf, SIZE) == 0);
    assert(memcmp(buf + 0x2086ae, "\x0d\xe0", 2) == 0);
    assert(memcmp(buf + 0xd9ffa, "\x00\xbf", 2) == 0);
    assert(memcmp(buf + 0x24c6d8, "\x01\x21", 2) == 0);
    assert(memcmp(buf + 0x24c700, "\x01\x21", 2) == 0);

    free(buf);
    free(copy);
}

static void check_real_binary(void)
{
    if (access(REAL_BINARY, R_OK) != 0)
    {
        printf("skipped real binary\n");
        return;
    }

    unlink("build/ims_rrm_patched");
    unlink("build/ims_rrm_bad");

    assert(ims_patch_file(REAL_BINARY, "build/ims_rrm_patched") == 0);
    assert(access("build/ims_rrm_patched", X_OK) == 0);

    /* Already patched: left alone */
    assert(ims_patch_file(REAL_BINARY, "build/ims_rrm_patched") == 0);

    /* Not the known build */
    assert(ims_patch_file("build/test_ims_patch", "build/ims_rrm_bad") == -1);
    assert(access("build/ims_rrm_bad", F_OK) != 0);
    assert(access("build/ims_rrm_bad.tmp", F_OK) != 0);
}

int main(void)
{
    check_buffer();
    check_real_binary();
    printf("test_ims_patch: OK\n");
    return 0;
}
