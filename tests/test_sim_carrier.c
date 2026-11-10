/* Run with `make test`. Builds system_state.c against a fake SIM status file. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The real header needs QNX's slog2. */
#define LOG_UTILS_H
#define LOG_VERBOSE(...) ((void)0)
void log_msg(const char *fmt, ...);
void bbe_exit_fail(const char *err_msg, int code);

#include "system_state.c"

static const char *g_status = "";
static int g_logged;

void log_msg(const char *fmt, ...)
{
    (void)fmt;
    g_logged++;
}

void bbe_exit_fail(const char *err_msg, int code)
{
    (void)err_msg, (void)code;
}

void exit_if_stopped(void)
{
}

void fix_published_carrier_xml(void)
{
}

int read_file(const char *path, char *buf, size_t max_len)
{
    (void)path;
    snprintf(buf, max_len, "%s", g_status);
    return (int)strlen(buf);
}

/* One "name:encoding:value" line per call, like the real parser's attributes. */
int ppsparse(char **pbuf, const char *objnames[], const char *attrnames[], pps_attrib_t *info, int flags)
{
    (void)objnames, (void)attrnames, (void)flags;
    char *line = *pbuf;
    if (!*line)
        return PPS_END;

    char *end = strchr(line, '\n');
    if (end)
        *end++ = '\0';
    *pbuf = end ? end : line + strlen(line);

    char *enc = strchr(line, ':');
    *enc++ = '\0';
    info->attr_name = line;
    info->value = strchr(enc, ':') + 1;
    return PPS_ATTRIBUTE;
}

int main(void)
{
    char mcc[4], mnc[4], gid1[41];

    const char *sim = "gid1::eeff\nhplmn:json:{\"mcc\":\"234\",\"mnc\":\"30\"}\nuicc_state:json:{\"state\":\"READY\"}\n";
    assert(parse_sim_carrier(sim, mcc, sizeof(mcc), mnc, sizeof(mnc), gid1, sizeof(gid1)) == 1);
    assert(strcmp(mcc, "234") == 0 && strcmp(mnc, "30") == 0 && strcmp(gid1, "eeff") == 0);

    /* No GID1: the carrier matches on the network alone. */
    const char *no_gid1 = "imsi_hplmn:json:{\"mcc\":\"310\",\"mnc\":\"260\"}\n";
    assert(parse_sim_carrier(no_gid1, mcc, sizeof(mcc), mnc, sizeof(mnc), gid1, sizeof(gid1)) == 1);
    assert(strcmp(mcc, "310") == 0 && strcmp(mnc, "260") == 0 && gid1[0] == '\0');

    assert(parse_sim_carrier("gid1::eeff\n", mcc, sizeof(mcc), mnc, sizeof(mnc), gid1, sizeof(gid1)) == 0);

    /* The daemon started on this SIM; sim_carrier_changed() compares against it. */
    g_status = sim;
    assert(detect_sim_carrier() == 1);
    assert(!sim_carrier_changed() && g_logged == 0);

    g_status = "gid1::eeff\nhplmn:json:{\"mcc\":\"234\",\"mnc\":\"15\"}\nuicc_state:json:{\"state\":\"READY\"}\n";
    assert(sim_carrier_changed() && g_logged == 1);

    g_status = "gid1::aa00\nhplmn:json:{\"mcc\":\"234\",\"mnc\":\"30\"}\nuicc_state:json:{\"state\":\"READY\"}\n";
    assert(sim_carrier_changed());

    /* A SIM that is still starting up reports nothing usable yet. */
    g_status = "hplmn:json:{\"mcc\":\"234\",\"mnc\":\"15\"}\nuicc_state:json:{\"state\":\"PIN_REQUIRED\"}\n";
    assert(!sim_carrier_changed());
    g_status = "uicc_state:json:{\"state\":\"READY\"}\n";
    assert(!sim_carrier_changed());

    printf("test_sim_carrier: OK\n");
    return 0;
}
