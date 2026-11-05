#include "carrier_apply.h"
#include "sys_utils.h"
#include "carrier_lookup.h"
#include "carrier_profile.h"
#include "ims_carrier_db.h"
#include "system_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define CARRIER_DB_MAX_AGE_SECONDS (30 * 24 * 3600)

static int g_built_carrier_id = -1;

/*
 * Prefers a hand-written XML for this SIM's carrier ID, then one built on the phone. A SIM with no
 * carrier ID can only use the optional default XML, reported through carrier_id as -1.
 */
static int find_carrier_xml(char *out_path, size_t max_len, int *carrier_id)
{
    *carrier_id = ims_get_carrier_id(g_mcc, g_mnc, g_gid1);
    if (*carrier_id > 0)
    {
        snprintf(out_path, max_len, "%s/carrier-%d-volte.xml", g_data_path, *carrier_id);
        if (access(out_path, R_OK) == 0)
            return 1;

        char apns_path[600];
        snprintf(apns_path, sizeof(apns_path), "%s/" CARRIER_APNS_NAME, g_data_path);
        snprintf(out_path, max_len, "%s/carrier-auto-%d-volte.xml", g_data_path, *carrier_id);

        /* Build once per SIM and refresh. */
        if (*carrier_id == g_built_carrier_id && access(out_path, R_OK) == 0)
            return 1;

        if (ims_build_carrier_xml(*carrier_id, CARRIER_CONFIG_DIR, apns_path, out_path) == 0)
        {
            g_built_carrier_id = *carrier_id;
            log_msg("[BBE] CARRIER_XML_BUILT: id=%d", *carrier_id);
            return 1;
        }
    }

    *carrier_id = -1;
    snprintf(out_path, max_len, "%s/carrier-default-volte.xml", g_data_path);
    if (access(out_path, R_OK) != 0)
        return 0;

    log_msg("[BBE] CARRIER_XML_FALLBACK: mcc=%s mnc=%s", g_mcc, g_mnc);
    return 1;
}

/* The data service refuses every APN change while this override is set. */
static void publish_override_xml(const char *carrier_xml)
{
    char cmd_buf[576];

    chmod(carrier_xml, 0644);
    LOG_VERBOSE("Carrier XML resolved to: %s", carrier_xml);

    snprintf(cmd_buf, sizeof(cmd_buf), "cdsc_xml::%s\n", carrier_xml);
    append_file(PPS_DATA_PATH, cmd_buf);
}

static void clear_override_xml(void)
{
    char status[4096];
    if (read_file(PPS_DATA_PATH, status, sizeof(status)) >= 0 && strstr(status, "cdsc_xml::/"))
        append_file(PPS_DATA_PATH, "cdsc_xml::\n");
}

static int path_linked_to(const char *path, const char *target)
{
    char current[600];
    ssize_t len = readlink(path, current, sizeof(current) - 1);
    if (len <= 0)
        return 0;

    current[len] = '\0';
    return strcmp(current, target) == 0;
}

/* A path manager link doesn't survive a reboot, so every start makes it again. */
static int link_path(char *target, char *path)
{
    char log_file[512];

    if (path_linked_to(path, target))
        return 1;

    snprintf(log_file, sizeof(log_file), "%s/carrier-link.log", RUN_PATH);
    char *args[] = {"/bin/ln", "-sP", target, path, NULL};
    run_cmd_spawn(args, log_file);
    return path_linked_to(path, target);
}

/*
 * The addon starts long after carrier_mgr looked the SIM up, and nothing makes it look again short
 * of its refreshBranding path, which can also remove carrier apps. So this publishes what its lookup
 * would have found: the same values it would write, read the same way by cellular-control and
 * ims_rrm. If it does look again, the links make it find the same answer.
 */
static int publish_home_carrier(int carrier_id)
{
    char status[1024];
    char wanted_id[32];
    char config[96];
    char cmd_buf[512];

    if (read_file(PPS_CARRIER_MGR_PATH, status, sizeof(status)) < 0)
        return 0;

    const char *home = strstr(status, "true_home_carrier:json:");
    if (!home)
        return 0;

    snprintf(wanted_id, sizeof(wanted_id), "\"id\":%d,", carrier_id);
    snprintf(config, sizeof(config), "/etc/carrier/config/%d.xml", carrier_id);
    if (strstr(home, wanted_id) && strstr(home, config))
        return 1;

    const char *npc = strstr(home, "\"npc\":");
    if (!npc)
        return 0;

    npc += 6;
    snprintf(cmd_buf, sizeof(cmd_buf),
             "true_home_carrier:json:{\"mcc\":%d,\"mnc\":%d,\"npc\":%.*s,\"id\":%d,"
             "\"preloadedConfigFile\":\"%s\",\"downloadedConfigFile\":\"\"}\n",
             atoi(g_mcc), atoi(g_mnc), (int)strspn(npc, "-0123456789"), npc, carrier_id, config);
    append_file(PPS_CARRIER_MGR_PATH, cmd_buf);
    log_msg("[BBE] CARRIER_PUBLISHED: id=%d", carrier_id);
    return 1;
}

/*
 * Serves the profile as the carrier's normal config, which leaves the APN editable in Settings.
 * Safe to repeat: every step does nothing if it's already in place. The override stays as the way
 * out if that fails, since VoLTE matters more than the editable APN.
 */
int apply_carrier_xml(void)
{
    char carrier_xml[512];
    char config_link[96];
    int carrier_id;

    if (!find_carrier_xml(carrier_xml, sizeof(carrier_xml), &carrier_id))
        return 0;

    if (carrier_id > 0)
    {
        chmod(carrier_xml, 0644);
        snprintf(config_link, sizeof(config_link), CARRIER_CONFIG_DIR "/%d.xml", carrier_id);
        if (link_path(carrier_xml, config_link) && publish_home_carrier(carrier_id))
        {
            clear_override_xml();
            return 1;
        }
        log_msg("[BBE] CARRIER_LINK_FAILED: id=%d", carrier_id);
    }

    publish_override_xml(carrier_xml);
    return 1;
}

/* A wrong XML can keep LTE down, so replacing it can't wait for LTE. */
void fix_published_carrier_xml(void)
{
    if (!pps_check_attribute_contains(PPS_RADIO_PATH, "network_technology", "lte"))
        apply_carrier_xml();
}

/* --- Carrier list provisioning --- */

void link_carrier_db(void)
{
    char target[600];
    char path[] = CARRIER_IDS_PATH;
    snprintf(target, sizeof(target), "%s/" CARRIER_DB_NAME, g_data_path);

    if (access(target, R_OK) != 0 || path_linked_to(path, target))
        return;

    log_msg(link_path(target, path) ? "[BBE] CARRIER_DB_LINKED" : "[BBE] CARRIER_DB_LINK_FAILED");
}

/*
 * ponytail: blocks startup for at most two 60 s downloads, only while the list is missing or
 * stale. Move it to a thread if that delay shows up on real starts. With no working APN and no
 * Wi-Fi the download can't happen, so the stock list and the default XML stay in use until it can.
 */
int refresh_carrier_db(int force)
{
    char db[600], stock[600], apns[600];
    struct stat st;

    snprintf(db, sizeof(db), "%s/" CARRIER_DB_NAME, g_data_path);
    snprintf(stock, sizeof(stock), "%s/" CARRIER_STOCK_NAME, g_data_path);
    snprintf(apns, sizeof(apns), "%s/" CARRIER_APNS_NAME, g_data_path);

    if (!force && stat(db, &st) == 0 && time(NULL) - st.st_mtime < CARRIER_DB_MAX_AGE_SECONDS)
        return 0;

    if (!path_linked_to(CARRIER_IDS_PATH, db) && ims_carrier_db_save_stock(CARRIER_IDS_PATH, stock) != 0)
    {
        log_msg("[BBE] CARRIER_DB_STOCK_COPY_FAILED");
        return -1;
    }

    if (ims_carrier_db_fetch(stock, db, apns) != 0)
    {
        log_msg("[BBE] CARRIER_DB_FETCH_FAILED");
        return -1;
    }

    log_msg("[BBE] CARRIER_DB_UPDATED");
    g_built_carrier_id = -1;
    link_carrier_db();
    return 0;
}
