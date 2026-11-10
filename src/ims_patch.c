#include "ims_patch.h"
#include "sys_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/pathmgr.h>

#define SYSTEM_PATH "/radio/bin/ims_rrm"
#define IMS_RRM_SIZE 4787852 /* size of ims_rrm on the only rooted build, patched or not */
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
};

#define PATCH_COUNT (sizeof(patches) / sizeof(patches[0]))

/* A site may already hold its patched bytes, since other addons copy and stack on our copy.
   Anything else at a site means a different build, and nothing is touched. */
int ims_patch_buffer(unsigned char *buf, size_t len)
{
    int changed = 0;
    size_t i;

    for (i = 0; i < PATCH_COUNT; i++)
    {
        if (patches[i].off + 2 > len ||
            (memcmp(buf + patches[i].off, patches[i].from, 2) != 0 &&
             memcmp(buf + patches[i].off, patches[i].to, 2) != 0))
            return -1;
    }

    for (i = 0; i < PATCH_COUNT; i++)
    {
        if (memcmp(buf + patches[i].off, patches[i].from, 2) == 0)
        {
            memcpy(buf + patches[i].off, patches[i].to, 2);
            changed = 1;
        }
    }

    return changed;
}

/* The whole file in memory, or NULL unless it is exactly IMS_RRM_SIZE bytes. */
static unsigned char *read_exact(const char *path)
{
    unsigned char *buf = malloc(IMS_RRM_SIZE + 1);
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
    while (len <= IMS_RRM_SIZE)
    {
        n = read(fd, buf + len, IMS_RRM_SIZE + 1 - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += n;
    }
    close(fd);

    if (n < 0 || len != IMS_RRM_SIZE)
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

/* src and dst may be the same file: src is read fully first and dst is replaced by a rename. */
int ims_patch_file(const char *src, const char *dst)
{
    unsigned char *buf = read_exact(src);
    int rc;

    if (!buf)
        return -1;

    rc = ims_patch_buffer(buf, IMS_RRM_SIZE);
    if (rc == 1 && write_replace(dst, buf, IMS_RRM_SIZE) != 0)
        rc = -1;

    free(buf);
    return rc;
}

/* The installer links SYSTEM_PATH to <init>/ims_rrm at boot, and another addon installed later can
   link its own patched copy over ours. A reinstall copies the file onto itself, which leaves it
   empty (or stale, if ims_rrm was running). So check whatever SYSTEM_PATH resolves to before each
   start and rebuild our copy if it is unusable. */
const char *ims_rrm_binary(void)
{
    static char path[PATH_BUF];
    char trust[PATH_BUF + 1];
    int rc;

    snprintf(path, sizeof(path), "%s/ims_rrm", g_init_path);

    rc = ims_patch_file(SYSTEM_PATH, path);
    if (rc < 0)
    {
        /* Drops a link to a broken copy. The real file on the read-only rcfs can't be deleted. */
        unlink(SYSTEM_PATH);
        rc = ims_patch_file(SYSTEM_PATH, path);
    }
    if (rc < 0)
    {
        log_msg("[BBE] IMS_RRM: unpatched (unknown build)");
        return SYSTEM_PATH;
    }

    /* Already carries our bytes, maybe in another addon's copy stacked on ours. */
    if (rc == 0)
    {
        log_msg("[BBE] IMS_RRM: patched");
        return SYSTEM_PATH;
    }

    /* Files written at runtime are untrusted and can't be executed.
       The addon installer trusts its own files the same way. */
    snprintf(trust, sizeof(trust), "!%s", path);
    char *argv[] = {"/proc/boot/pathtrust", trust, NULL};
    if (run_cmd_spawn(argv, NULL) != 0)
    {
        log_msg("[BBE] IMS_RRM: unpatched (trust failed)");
        return SYSTEM_PATH;
    }

    /* Same link the installer makes at boot. */
    unlink(SYSTEM_PATH);
    if (pathmgr_symlink(path, SYSTEM_PATH) != 0)
    {
        log_msg("[BBE] IMS_RRM: link failed: %s", strerror(errno));
        return path;
    }

    log_msg("[BBE] IMS_RRM: repaired");
    return SYSTEM_PATH;
}
