#include "ims_patch.h"
#include "sys_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define STOCK_PATH "/radio/bin/ims_rrm"
#define STOCK_SIZE 4787852
#define STOCK_CRC 0xe8c52819UL   /* crc32 of the whole stock file */
#define PATCHED_CRC 0x67719fe8UL /* crc32 after all patches */
#define PATH_BUF (512 + 16)

typedef struct
{
    size_t off;
    unsigned char from[2];
    unsigned char to[2];
} patch_t;

static const patch_t patches[] = {
    /* SDP parser: an e= line it can't parse aborted the whole answer and hung up the call
       (Telus sends e=imsstirshaken@telus.com, valid per RFC 4566).
       `cbnz r0` becomes `b`, so the line is skipped. */
    {0x2086ae, {0x68, 0xb9}, {0x0d, 0xe0}},
    /* Remote numbers without a '+' were shown as user@host (Viettel national format).
       `beq` becomes `nop`, so the user part is always used. */
    {0xd9ffa, {0x08, 0xd0}, {0x00, 0xbf}},
    /* The SDP answer carried our own AMR/AMR-WB mode-set instead of the offered one.
       Telus offers mode-set=7 in an UPDATE and rejects 0,2,4,7 with 406, killing the call.
       ims_rrm can copy the offered mode-set, gated on AMR[WB].CopyRemoteModeSet, but only
       the '3GPP' scheme sets those to 1, and onMobileTechnologyChanged renames an unknown
       scheme ('3GPP' included) to 'Vodafone' first, so the gate always read its default 0.
       `movs r1, #0` becomes `movs r1, #1`: the default for AMR-WB, then AMR. */
    {0x24c6d8, {0x00, 0x21}, {0x01, 0x21}},
    {0x24c700, {0x00, 0x21}, {0x01, 0x21}},
    /* TEST: outgoing calls went to sip:<digits>@ims.mncXXX.mccYYY.3gppnetwork.org;user=phone,
       the domain derived from the IMSI, and EE and Telekom DE answer 503 "(1:223)"; their
       identities are in their own domains. The dial pattern's literal (pc-relative, at c28be)
       now points at the stock string "tel:<formatted-digits>" (0x3743df) that the built-in
       operator schemes use, so the Request-URI carries no domain. */
    {0xc2bb8, {0xa3, 0x71}, {0x19, 0x1b}},
    {0xc2bba, {0x29, 0x00}, {0x2b, 0x00}},
};

#define PATCH_COUNT (sizeof(patches) / sizeof(patches[0]))

int ims_patch_buffer(unsigned char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < PATCH_COUNT; i++)
    {
        if (patches[i].off + 2 > len || memcmp(buf + patches[i].off, patches[i].from, 2) != 0)
            return -1;
    }

    for (i = 0; i < PATCH_COUNT; i++)
        memcpy(buf + patches[i].off, patches[i].to, 2);

    return 0;
}

/* The whole file in memory, or NULL unless it is exactly STOCK_SIZE bytes. */
static unsigned char *read_exact(const char *path)
{
    unsigned char *buf = malloc(STOCK_SIZE + 1);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t len = 0;
    ssize_t n = 0;

    if (!buf || fd < 0)
    {
        free(buf);
        if (fd >= 0)
            close(fd);
        return NULL;
    }

    /* One byte more than expected, so a longer file shows up as a size mismatch. */
    while (len <= STOCK_SIZE)
    {
        n = read(fd, buf + len, STOCK_SIZE + 1 - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += n;
    }
    close(fd);

    if (n < 0 || len != STOCK_SIZE)
    {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Writes <dst>.tmp, then renames it over dst, so dst is never half written. */
static int write_replace(const char *dst, const unsigned char *buf, size_t len)
{
    char tmp[PATH_BUF];
    size_t done = 0;
    int ok;
    int fd;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", dst) >= (int)sizeof(tmp))
        return -1;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (fd < 0)
        return -1;

    while (done < len)
    {
        ssize_t n = write(fd, buf + done, len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += n;
    }

    ok = (close(fd) == 0 && done == len && rename(tmp, dst) == 0);
    if (!ok)
        unlink(tmp);
    return ok ? 0 : -1;
}

int ims_patch_file(const char *src, const char *dst)
{
    unsigned char *buf = read_exact(dst);
    int rc = -1;

    if (buf)
    {
        int patched = crc32(0L, buf, STOCK_SIZE) == PATCHED_CRC;
        free(buf);
        if (patched)
            return 0;
    }

    buf = read_exact(src);
    if (!buf)
        return -1;

    if (crc32(0L, buf, STOCK_SIZE) == STOCK_CRC && ims_patch_buffer(buf, STOCK_SIZE) == 0)
        rc = write_replace(dst, buf, STOCK_SIZE);

    free(buf);
    return rc;
}

const char *ims_rrm_binary(void)
{
    static char path[PATH_BUF];
    char switch_file[PATH_BUF];
    char trust[PATH_BUF + 1];

    /* Manual switch for testers if the patched copy misbehaves. */
    snprintf(switch_file, sizeof(switch_file), "%s/stock-ims-rrm", g_data_path);
    if (access(switch_file, F_OK) == 0)
    {
        log_msg("[BBE] IMS_RRM: stock (switch)");
        return STOCK_PATH;
    }

    snprintf(path, sizeof(path), "%s/ims_rrm", g_init_path);
    if (ims_patch_file(STOCK_PATH, path) != 0)
    {
        log_msg("[BBE] IMS_RRM: stock (unknown build)");
        return STOCK_PATH;
    }

    /* Files written at runtime are untrusted and can't be executed.
       The addon installer trusts its own files the same way. */
    snprintf(trust, sizeof(trust), "!%s", path);
    char *argv[] = {"/proc/boot/pathtrust", trust, NULL};
    if (run_cmd_spawn(argv, NULL) != 0)
    {
        log_msg("[BBE] IMS_RRM: stock (trust failed)");
        return STOCK_PATH;
    }

    log_msg("[BBE] IMS_RRM: patched");
    return path;
}
