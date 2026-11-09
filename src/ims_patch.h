#ifndef IMS_PATCH_H
#define IMS_PATCH_H

#include <stddef.h>

/* Applies the byte patches to a copy of the stock ims_rrm. -1 and no change if any byte differs. */
int ims_patch_buffer(unsigned char *buf, size_t len);

/* Makes dst the patched copy of src. 0 when dst holds the patched build, -1 if src is not the known build. */
int ims_patch_file(const char *src, const char *dst);

/* Path ims_rrm should be started from: the patched copy, or the stock file as the fallback. */
const char *ims_rrm_binary(void);

#endif /* IMS_PATCH_H */
