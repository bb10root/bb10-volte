#ifndef IMS_PATCH_H
#define IMS_PATCH_H

#include <stddef.h>

/* Applies the byte patches still missing. 1 if it changed bytes, 0 if all are there already,
   -1 and no change if a site holds neither the stock nor the patched bytes. */
int ims_patch_buffer(unsigned char *buf, size_t len);

/* Writes dst as the patched src. 1 when written, 0 when src already carries the patches (dst is
   left alone), -1 if src is not the known build. src and dst may be the same file. */
int ims_patch_file(const char *src, const char *dst);

/* Path ims_rrm should be started from: /radio/bin/ims_rrm, which resolves to a patched copy (ours,
   rebuilt if a reinstall emptied it, or another addon's) or is stock on an unknown build.
   Our own copy instead if the link to it can't be made. */
const char *ims_rrm_binary(void);

#endif /* IMS_PATCH_H */
