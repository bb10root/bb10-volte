/* Stand-in for QNX's <sys/pathmgr.h>, only what carrier_apply.c uses. */
#ifndef FAKE_SYS_PATHMGR_H
#define FAKE_SYS_PATHMGR_H

int pathmgr_symlink(const char *path, const char *symlink);

#endif
