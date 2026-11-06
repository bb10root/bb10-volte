/* Run with `make test`. Builds carrier_apply.c against a fake PPS, since it needs no phone for this. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The real header needs QNX's slog2. */
#define LOG_UTILS_H
#define LOG_VERBOSE(...) ((void)0)
void log_msg(const char *fmt, ...);

static char g_link[600];
static int g_sleeps;
static ssize_t fake_readlink(char *buf, size_t size);
#define readlink(path, buf, size) fake_readlink(buf, size)
#define access(path, mode) 0
#define sleep(seconds) (g_sleeps++, 0)

#include "../src/carrier_apply.c"

#undef readlink
#undef access
#undef sleep

#define XML "/data/carrier-70-volte.xml"
#define STOCK_PATH "/etc/carrier/config/70.xml"

char g_mcc[4] = "621";
char g_mnc[4] = "50";
char g_gid1[41] = "";
char g_data_path[512] = "/data";

static char g_home[256];
static char g_override[600];
static char g_signature[600];
static char g_last_log[128];
static int g_responsive;
static int g_override_writes;
static int g_home_writes;

static ssize_t fake_readlink(char *buf, size_t size)
{
    size_t len = strlen(g_link);
    assert(len < size);
    memcpy(buf, g_link, len);
    return (ssize_t)len;
}

void log_msg(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_log, sizeof(g_last_log), fmt, args);
    va_end(args);
}

int read_file(const char *path, char *buf, size_t max_len)
{
    if (strcmp(path, PPS_DATA_PATH) == 0)
        return snprintf(buf, max_len, "cdsc_xml::%s\n", g_override);
    return snprintf(buf, max_len, "%s", g_home);
}

/* Behaves as seen on the phone: cellular-control reloads when the override changes. */
int append_file(const char *path, const char *content)
{
    if (strcmp(path, PPS_DATA_PATH) == 0)
    {
        g_override_writes++;
        sscanf(content, "cdsc_xml::%599[^\n]", g_override);
        if (strcmp(content, "cdsc_xml::\n") == 0)
            g_override[0] = '\0';
        if (g_responsive)
            snprintf(g_signature, sizeof(g_signature), "%s", g_override[0] ? g_override : STOCK_PATH);
    }
    else
    {
        g_home_writes++;
        snprintf(g_home, sizeof(g_home), "%s", content);
    }
    return 0;
}

int pps_check_attribute_contains(const char *pps_path, const char *attr_name, const char *expected_val)
{
    (void)pps_path;
    (void)attr_name;
    return strstr(g_signature, expected_val) != NULL;
}

int run_cmd_spawn(char *const argv[], const char *log_path)
{
    (void)log_path;
    snprintf(g_link, sizeof(g_link), "%s", argv[2]);
    return 0;
}

int ims_get_carrier_id(const char *mcc, const char *mnc, const char *gid1)
{
    (void)mcc;
    (void)mnc;
    (void)gid1;
    return 70;
}

int ims_build_carrier_xml(int carrier_id, const char *config_dir, const char *apns_path, const char *out_path)
{
    (void)carrier_id;
    (void)config_dir;
    (void)apns_path;
    (void)out_path;
    return 0;
}

int ims_carrier_db_save_stock(const char *src, const char *dst)
{
    (void)src;
    (void)dst;
    return 0;
}

int ims_carrier_db_fetch(const char *stock_path, const char *out_db, const char *out_apns)
{
    (void)stock_path;
    (void)out_db;
    (void)out_apns;
    return 0;
}

static void start_daemon(const char *home, int responsive)
{
    snprintf(g_home, sizeof(g_home), "%s", home);
    snprintf(g_signature, sizeof(g_signature), "%s", STOCK_PATH);
    g_override[0] = g_link[0] = g_last_log[0] = '\0';
    g_responsive = responsive;
    g_override_writes = g_home_writes = g_sleeps = 0;
    g_config_reloaded = 0;
}

static const char k_home_stock[] =
    "true_home_carrier:json:{\"mcc\":621,\"mnc\":50,\"npc\":1,\"id\":70,"
    "\"preloadedConfigFile\":\"/etc/carrier/config/70.xml\",\"downloadedConfigFile\":\"\"}\n";
static const char k_home_unknown[] =
    "true_home_carrier:json:{\"mcc\":621,\"mnc\":50,\"npc\":1,\"id\":-1,"
    "\"preloadedConfigFile\":\"\",\"downloadedConfigFile\":\"\"}\n";

int main(void)
{
    /* A stock-listed carrier: every value already matches, and it must still reload. */
    start_daemon(k_home_stock, 1);
    assert(apply_carrier_xml() == 1);
    assert(strcmp(g_link, XML) == 0);
    assert(g_home_writes == 0);
    assert(g_override_writes == 2);
    assert(g_override[0] == '\0');
    assert(strcmp(g_signature, STOCK_PATH) == 0);
    assert(strcmp(g_last_log, "[BBE] CARRIER_RELOADED") == 0);

    /* Startup can apply twice: the second time must not reload again. */
    assert(apply_carrier_xml() == 1);
    assert(g_override_writes == 2);

    /* A carrier the lookup didn't find reloads the same way. */
    start_daemon(k_home_unknown, 1);
    assert(apply_carrier_xml() == 1);
    assert(g_home_writes == 1);
    assert(g_override_writes == 2);
    assert(g_override[0] == '\0');

    /* If cellular-control never answers, startup carries on with APN edits still possible. */
    start_daemon(k_home_stock, 0);
    assert(apply_carrier_xml() == 1);
    assert(g_override[0] == '\0');
    assert(g_sleeps <= CARRIER_RELOAD_WAIT_SECONDS);
    assert(strcmp(g_last_log, "[BBE] CARRIER_RELOAD_TIMEOUT") == 0);

    puts("carrier reload tests passed");
    return 0;
}
