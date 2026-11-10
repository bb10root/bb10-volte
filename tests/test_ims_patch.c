/* Run with `make test`. Builds ims_patch.c without QNX's slog2 and the daemon. */
#include <assert.h>
#include <fcntl.h>
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

int pathmgr_symlink(const char *path, const char *symlink)
{
    (void)path;
    (void)symlink;
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
    assert(ims_patch_buffer(buf, SIZE) == 1);
    assert(memcmp(buf + 0x2086ae, "\x0d\xe0", 2) == 0);
    assert(memcmp(buf + 0xd9ffa, "\x00\xbf", 2) == 0);
    assert(memcmp(buf + 0x24c6d8, "\x01\x21", 2) == 0);
    assert(memcmp(buf + 0x24c700, "\x01\x21", 2) == 0);

    /* Again: already patched, nothing changes */
    memcpy(copy, buf, SIZE);
    assert(ims_patch_buffer(buf, SIZE) == 0);
    assert(memcmp(buf, copy, SIZE) == 0);

    /* One site already patched, the others stock */
    for (size_t i = 1; i < COUNT; i++)
        put(buf, i, 0);
    assert(ims_patch_buffer(buf, SIZE) == 1);
    for (size_t i = 0; i < COUNT; i++)
        assert(memcmp(buf + patches[i].off, patches[i].to, 2) == 0);

    free(buf);
    free(copy);
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = malloc(IMS_RRM_SIZE + 1);

    assert(f && buf);
    *len = fread(buf, 1, IMS_RRM_SIZE + 1, f);
    fclose(f);
    return buf;
}

static void spit(const char *path, const unsigned char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");

    assert(f && fwrite(buf, 1, len, f) == len);
    fclose(f);
}

static void check_real_binary(void)
{
    unsigned char *stock, *patched, *other, *self;
    size_t len;

    if (access(REAL_BINARY, R_OK) != 0)
    {
        printf("skipped real binary\n");
        return;
    }

    unlink("build/ims_rrm_patched");
    unlink("build/ims_rrm_again");
    unlink("build/ims_rrm_bad");
    unlink("build/ims_rrm_empty");

    assert(ims_patch_file(REAL_BINARY, "build/ims_rrm_patched") == 1);
    assert(access("build/ims_rrm_patched", X_OK) == 0);

    /* Already patched: reported, dst left alone */
    assert(ims_patch_file("build/ims_rrm_patched", "build/ims_rrm_again") == 0);
    assert(access("build/ims_rrm_again", F_OK) != 0);

    /* A reinstall copied the file onto itself: src and dst are the same file, one site is stock */
    patched = slurp("build/ims_rrm_patched", &len);
    assert(len == IMS_RRM_SIZE);
    self = malloc(len);
    memcpy(self, patched, len);
    memcpy(self + patches[1].off, patches[1].from, 2);
    spit("build/ims_rrm_self", self, len);
    assert(ims_patch_file("build/ims_rrm_self", "build/ims_rrm_self") == 1);
    free(self);
    self = slurp("build/ims_rrm_self", &len);
    assert(len == IMS_RRM_SIZE && memcmp(self, patched, len) == 0);
    free(self);

    /* Another addon's patch elsewhere in the file survives ours */
    stock = slurp(REAL_BINARY, &len);
    other = malloc(len);
    memcpy(other, stock, len);
    other[0x1000] ^= 0xff;
    spit("build/ims_rrm_other", other, len);
    assert(ims_patch_file("build/ims_rrm_other", "build/ims_rrm_stacked") == 1);
    free(other);
    other = slurp("build/ims_rrm_stacked", &len);
    assert(len == IMS_RRM_SIZE && other[0x1000] == (unsigned char)(stock[0x1000] ^ 0xff));
    for (size_t i = 0; i < COUNT; i++)
        assert(memcmp(other + patches[i].off, patches[i].to, 2) == 0);
    free(other);
    free(stock);
    free(patched);

    /* Not the known build, or a copy a reinstall emptied */
    assert(ims_patch_file("build/test_ims_patch", "build/ims_rrm_bad") == -1);
    assert(access("build/ims_rrm_bad", F_OK) != 0);
    assert(access("build/ims_rrm_bad.tmp", F_OK) != 0);
    spit("build/ims_rrm_empty", (const unsigned char *)"", 0);
    assert(ims_patch_file("build/ims_rrm_empty", "build/ims_rrm_bad") == -1);
    assert(access("build/ims_rrm_bad", F_OK) != 0);
}

int main(void)
{
    check_buffer();
    check_real_binary();
    printf("test_ims_patch: OK\n");
    return 0;
}
