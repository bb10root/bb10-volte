/* Reversible io-pkt loader for the software provider already built into BB10.
 * No packet hooks, patches, hardware drivers, or cipher implementations.
 * Explicit mount options must be either enable or disable.
 */
#include <stdint.h>
#include <errno.h>
#include <string.h>

extern int swcr_id;
extern void swcryptoattach(int);
extern int crypto_unregister_all(uint32_t);

static int provider_entry(void *handle, void *self, char *options)
{
    (void)handle;
    (void)self;
    if (!options) return EINVAL;
    /* Read-only probe: mount succeeds only when the provider is registered. */
    if (strcmp(options, "status") == 0) return swcr_id >= 0 ? 0 : ENODEV;
    if (strcmp(options, "enable") == 0) {
        if (swcr_id != -1) return 0;
        swcryptoattach(0);
        return swcr_id >= 0 ? 0 : EIO;
    }
    if (strcmp(options, "disable") == 0) {
        int result;
        if (swcr_id == -1) return 0;
        result = crypto_unregister_all((uint32_t)swcr_id);
        if (result == 0) swcr_id = -1;
        return result;
    }
    return EINVAL;
}

struct {
    uint32_t version;
    uint32_t flags;
    int (*entry)(void *, void *, char *);
} iopkt_drvr_entry = {1, 0, provider_entry};
