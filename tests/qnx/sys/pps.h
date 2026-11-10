/* Stand-in for QNX's <sys/pps.h>, only what system_state.c uses. */
#ifndef FAKE_SYS_PPS_H
#define FAKE_SYS_PPS_H

typedef struct
{
    const char *attr_name;
    const char *value;
} pps_attrib_t;

enum
{
    PPS_END = 0,
    PPS_ATTRIBUTE = 2
};

int ppsparse(char **pbuf, const char *objnames[], const char *attrnames[], pps_attrib_t *info, int flags);

#endif
