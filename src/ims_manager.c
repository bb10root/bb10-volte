#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <spawn.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/pps.h>
#include <sys/slog2.h>

#include "sys_utils.h"
#include "berry_monitor.h"
#include "ims_carrier_config.h"
#include "ims_carrier_db.h"

#define IR92_PATH "/var/dca/sources/ims/services/carrier_config/ims/ir92"
#define STATS_FILE_PATH "/var/run/bb10_volte_stats.txt"
#define CARRIER_DB_MAX_AGE_SECONDS (30 * 24 * 3600)

#define PPS_RADIO_PATH "/pps/services/cellular/radioctrl/status_public"
#define PPS_UICC_PATH "/pps/services/cellular/uicc/card0/status_private"
#define PPS_DATA_PATH "/pps/services/cds/cnice/status_persistent"
#define PPS_CARRIER_MGR_PATH "/pps/services/carriermanager/status_private"
#define PPS_RADIO_CONTROL_PATH "/pps/services/cellular/radioctrl/control"

#define INITIAL_BACKOFF_SEC 2
#define MAX_BACKOFF_SEC 64
#define MAX_RETRY_ATTEMPTS 6

#define IMS_CHECK_INTERVAL_SEC 10
#define IMS_DOWN_GRACE_SEC 60     /* Tolerated re-registration time before restarting IMS */
#define IMS_RETRY_INTERVAL_SEC 300 /* Wait after a reconnect sequence gives up */
#define REGISTER_WAIT_SEC 60
#define RADIO_RESET_MIN_INTERVAL_SEC 1800

/* network_service bits, as tested by ims_rrm and named by cellular-control */
#define NET_SERVICE_DATA_LTE 0x200000UL
#define NET_SERVICE_IMS_VOICE 0x2000000UL

enum
{
    FD_RADIO = 0,
    FD_UICC,
    FD_DATA,
    FD_COUNT
};

typedef struct
{
    int lte_ok;
    int sim_ok;
    int data_ok;
} system_state_t;

typedef struct
{
    unsigned int total_drops;
    unsigned int successful_reconnects;
    unsigned int failed_reconnects;
    double total_recovery_time_sec;
    struct timespec drop_time;
    time_t last_reset_time;
} connection_stats_t;

static char g_mcc[4] = {0};
static char g_mnc[4] = {0};
static char g_gid1[41] = {0};
static int g_built_carrier_id = -1;
static char g_ims_xml_path[256] = {0};
static int g_force_ims_voice = 0;

static pthread_t g_lte_thread;
static volatile int g_lte_monitor_running = 0;
static system_state_t g_current_state = {0};
static connection_stats_t g_stats = {0};

/* Set by signal handlers, acted on by the main and monitor threads. */
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_reset_stats_requested = 0;

static void exit_if_stopped(void);

/* --- Statistics Management --- */

void stats_reset(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.last_reset_time = time(NULL);

    slog2f(NULL, 0, SLOG2_INFO, "[STATS] Connection statistics counters reset.");
    log_msg("[BBE] STATS_RESET");
}

void stats_export_to_file(void)
{
    FILE *f = fopen(STATS_FILE_PATH, "w");
    if (!f)
    {
        LOG_VERBOSE("Failed to open stats file for writing: %s", STATS_FILE_PATH);
        return;
    }

    double avg_recovery = (g_stats.successful_reconnects > 0)
                              ? (g_stats.total_recovery_time_sec / g_stats.successful_reconnects)
                              : 0.0;

    char reset_str[64];
    struct tm *tm_info = localtime(&g_stats.last_reset_time);
    if (tm_info)
    {
        strftime(reset_str, sizeof(reset_str), "%Y-%m-%d %H:%M:%S", tm_info);
    }
    else
    {
        snprintf(reset_str, sizeof(reset_str), "N/A");
    }

    fprintf(f, "=== BB10 VoLTE Connection Statistics ===\n");
    fprintf(f, "Last Reset           : %s\n", reset_str);
    fprintf(f, "Total Connection Drops: %u\n", g_stats.total_drops);
    fprintf(f, "Successful Reconnects : %u\n", g_stats.successful_reconnects);
    fprintf(f, "Failed Reconnects     : %u\n", g_stats.failed_reconnects);
    fprintf(f, "Avg Recovery Time     : %.2f sec\n", avg_recovery);
    fprintf(f, "========================================\n");

    fclose(f);
}

void stats_print_cli(void)
{
    FILE *f = fopen(STATS_FILE_PATH, "r");
    if (f)
    {
        int ch;
        while ((ch = fgetc(f)) != EOF)
        {
            putchar(ch);
        }
        fclose(f);
    }
    else
    {
        printf("No active statistics file found at %s\n", STATS_FILE_PATH);
    }
}

static double timespec_diff_sec(const struct timespec *start, const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1e9;
}

static int g_drop_pending = 0;

static void stats_on_drop(void)
{
    g_drop_pending = 1;
    g_stats.total_drops++;
    clock_gettime(CLOCK_MONOTONIC, &g_stats.drop_time);
    stats_export_to_file();
}

static void stats_on_success(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    g_drop_pending = 0;
    double recovery_duration = timespec_diff_sec(&g_stats.drop_time, &now);
    g_stats.successful_reconnects++;
    g_stats.total_recovery_time_sec += recovery_duration;

    double avg_recovery_time = g_stats.total_recovery_time_sec / g_stats.successful_reconnects;

    slog2f(NULL, 0, SLOG2_INFO,
           "[STATS] IMS Restored in %.2fs | Drops: %u | Reconnects: %u | Failed: %u | Avg Recovery: %.2fs",
           recovery_duration,
           g_stats.total_drops,
           g_stats.successful_reconnects,
           g_stats.failed_reconnects,
           avg_recovery_time);

    log_msg("[BBE] STATS_OK: last=%.2fs avg=%.2fs drops=%u",
            recovery_duration, avg_recovery_time, g_stats.total_drops);

    stats_export_to_file();
}

static void stats_on_failure(void)
{
    g_stats.failed_reconnects++;

    slog2f(NULL, 0, SLOG2_WARNING,
           "[STATS] IMS Reconnect Failed | Drops: %u | Reconnects: %u | Failed: %u",
           g_stats.total_drops,
           g_stats.successful_reconnects,
           g_stats.failed_reconnects);

    stats_export_to_file();
}

/* --- PPS checks and parsing --- */

static int pps_check_attribute_contains(const char *pps_path, const char *attr_name, const char *expected_val)
{
    char buf[2048];
    if (read_file(pps_path, buf, sizeof(buf)) < 0)
        return 0;

    char *ptr = buf;
    pps_attrib_t info;
    const char *attrs[] = {attr_name, NULL};

    while (ppsparse(&ptr, NULL, attrs, &info, 0) > PPS_END)
    {
        if (info.attr_name && strcmp(info.attr_name, attr_name) == 0)
        {
            if (info.value && strstr(info.value, expected_val) != NULL)
            {
                return 1;
            }
        }
    }
    return 0;
}

static system_state_t get_current_system_state(void)
{
    system_state_t state;
    state.lte_ok = pps_check_attribute_contains(PPS_RADIO_PATH, "network_technology", "lte");
    state.sim_ok = pps_check_attribute_contains(PPS_UICC_PATH, "uicc_state", "READY");
    state.data_ok = pps_check_attribute_contains(PPS_DATA_PATH, "data_service_mode", "ON");
    return state;
}

static int is_state_ready(const system_state_t *st)
{
    return (st->lte_ok && st->sim_ok && st->data_ok);
}

static void log_state_transition(system_state_t old_st, system_state_t new_st)
{
    int old_ready = is_state_ready(&old_st);
    int new_ready = is_state_ready(&new_st);

    if (old_ready && !new_ready)
    {
        if (!new_st.lte_ok)
        {
            log_msg("[BBE] DROPPED: LTE_LOST");
            LOG_VERBOSE("Connection dropped: LTE network lost");
        }
        if (!new_st.sim_ok)
        {
            log_msg("[BBE] DROPPED: SIM_NOT_READY");
            LOG_VERBOSE("Connection dropped: SIM status changed from READY");
        }
        if (!new_st.data_ok)
        {
            log_msg("[BBE] DROPPED: DATA_OFF");
            LOG_VERBOSE("Connection dropped: Cellular data disabled");
        }
    }
    else if (!old_ready && new_ready)
    {
        log_msg("[BBE] RESTORED: ALL_SERVICES_OK");
        LOG_VERBOSE("All conditions satisfied (LTE, SIM, Data). Restoring IMS...");
    }
}

static int incoming_ready_pps(void)
{
    if (!pps_check_attribute_contains(PPS_IMS, "allowVoLteProvisioning", "1"))
        return 0;

    char buf2[256];
    return (read_file(IR92_PATH, buf2, sizeof(buf2)) >= 0 && strstr(buf2, "UsePreconditions:n:0") != NULL);
}

static int parse_json_key_value(const char *src, const char *key, char *out_val, size_t max_len)
{
    char search_pattern[32];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\":\"", key);

    const char *pos = strstr(src, search_pattern);
    if (!pos)
        return 0;

    pos += strlen(search_pattern);
    size_t i = 0;
    while (pos[i] >= '0' && pos[i] <= '9' && i < (max_len - 1))
    {
        out_val[i] = pos[i];
        i++;
    }
    out_val[i] = '\0';
    return i > 0;
}

static int detect_sim_carrier(void)
{
    char buf[2048];
    if (read_file(PPS_UICC_PATH, buf, sizeof(buf)) < 0)
        return 0;

    /* A SIM may have no GID1, and carriers then match on the network alone. */
    const char *gid1 = strstr(buf, "gid1::");
    g_gid1[0] = '\0';
    if (gid1)
    {
        size_t len = strspn(gid1 + 6, "0123456789abcdefABCDEF");
        if (len < sizeof(g_gid1))
        {
            memcpy(g_gid1, gid1 + 6, len);
            g_gid1[len] = '\0';
        }
    }

    const char *hplmn_pos = strstr(buf, "imsi_hplmn");
    if (!hplmn_pos)
        hplmn_pos = strstr(buf, "hplmn");

    if (hplmn_pos)
    {
        if (parse_json_key_value(hplmn_pos, "mcc", g_mcc, sizeof(g_mcc)) &&
            parse_json_key_value(hplmn_pos, "mnc", g_mnc, sizeof(g_mnc)))
        {
            return 1;
        }
    }
    return 0;
}

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
static int apply_carrier_xml(void)
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
static void fix_published_carrier_xml(void)
{
    if (!pps_check_attribute_contains(PPS_RADIO_PATH, "network_technology", "lte"))
        apply_carrier_xml();
}

static void wait_for_lte(void)
{
    int printed_log = 0;
    while (!pps_check_attribute_contains(PPS_RADIO_PATH, "network_technology", "lte"))
    {
        if (!printed_log)
        {
            LOG_VERBOSE("LTE network not available. Waiting for LTE connection...");
            log_msg("[BBE] WAITING_FOR_LTE");
            printed_log = 1;
        }
        exit_if_stopped();
        sleep(3);
    }

    if (printed_log)
    {
        LOG_VERBOSE("LTE network detected. Proceeding...");
        log_msg("[BBE] LTE_CONNECTED");
    }
}

/* Returns the first unmet precondition other than LTE, or NULL. */
static const char *ready_failure(void)
{
    if (!pps_check_attribute_contains("/pps/services/phone/public/status", "phone_state", "idle"))
        return "CALL_ACTIVE";

    if (!pps_check_attribute_contains(PPS_UICC_PATH, "uicc_state", "READY"))
        return "SIM_NOT_READY";

    if (!detect_sim_carrier())
        return "SIM_UNSUPPORTED";

    if (!pps_check_attribute_contains(PPS_DATA_PATH, "data_service_mode", "ON"))
        return "DATA_OFF";

    return NULL;
}

static void ready_check(void)
{
    const char *failure = ready_failure();
    if (failure)
        bbe_exit_fail(failure, 1);

    wait_for_lte();
}

/* Started at boot with nobody to retry: the SIM or data may just not be up yet. */
static void wait_until_ready(void)
{
    const char *failure;
    const char *logged = NULL;

    while ((failure = ready_failure()) != NULL)
    {
        if (failure != logged)
        {
            log_msg("[BBE] NOT_READY: %s", failure);
            logged = failure;
        }
        exit_if_stopped();
        sleep(3);
    }

    fix_published_carrier_xml();
    wait_for_lte();
}

/* --- Carrier list provisioning --- */

static void link_carrier_db(void)
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
static int refresh_carrier_db(int force)
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

/* --- Provider & IMS control --- */

static void provider_enable(void)
{
    char module_path[512];
    char log_file[512];

    snprintf(module_path, sizeof(module_path), "%s/devnp-volte-swcrypto.so", g_init_path);
    snprintf(log_file, sizeof(log_file), "%s/provider.log", RUN_PATH);

    char *args[] = {
        "mount",
        "-T", "io-pkt",
        "-o", "enable",
        module_path,
        NULL};

    if (run_cmd_spawn(args, log_file) != 0)
        bbe_exit_fail("PROVIDER_FAILED", 1);
}

static void stop_ims(void)
{
    if (find_pid_by_name("ims_rrm", 0) > 0)
    {
        if (slay_by_cmdline_graceful("ims_rrm", 15000) != 0)
        {
            bbe_exit_fail("IMS_STOP_FAILED", 1);
        }
        sleep(2);
    }
}

static void start_ims(void)
{
    char log_file[512];
    snprintf(log_file, sizeof(log_file), "%s/ims-start.log", RUN_PATH);

    int fd = open(log_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (fd >= 0)
    {
        posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, fd);
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);

    sigset_t sigdefault;
    sigemptyset(&sigdefault);
    sigaddset(&sigdefault, SIGTERM);
    sigaddset(&sigdefault, SIGINT);

    posix_spawnattr_setsigdefault(&attr, &sigdefault);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);

    char *args[] = {
        "on", "-d", "/radio/bin/ims_rrm",
        "-U", "453:31,80,387,427,521,1001",
        NULL};

    /* `on` execs ims_rrm, so it is our child. Reap one that crashed or was killed. */
    static pid_t last_pid = 0;
    if (last_pid > 0)
        waitpid(last_pid, NULL, WNOHANG);

    pid_t pid;
    int status = posix_spawnp(&pid, "on", &actions, &attr, args, NULL);

    if (status != 0)
    {
        LOG_VERBOSE("Failed to spawn ims_rrm process, error: %d", status);
    }
    else
    {
        last_pid = pid;
        LOG_VERBOSE("Spawned ims_rrm successfully with PID %d", pid);
    }

    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    if (fd >= 0)
    {
        close(fd);
    }
}

static void apply_volte_provisioning(void)
{
    if (g_ims_xml_path[0] == '\0')
    {
        if (ims_resolve_config_path(g_mcc, g_mnc, g_gid1, g_ims_xml_path, sizeof(g_ims_xml_path)) != 0)
        {
            snprintf(g_ims_xml_path, sizeof(g_ims_xml_path), "/var/ims/carrier/imsconfig_0%s.xml", g_mnc);
        }
    }

    /* Not fatal: the registration and incoming checks decide the outcome. */
    if (ims_enable_volte_provisioning(g_ims_xml_path) != 0)
    {
        log_msg("[BBE] PROVISIONING_FAILED: %s", g_ims_xml_path);
        return;
    }

    LOG_VERBOSE("VoLTE provisioning applied via XML: %s", g_ims_xml_path);
}

static void write_dca_configs(void)
{
    mkdir_p("/var/dca/sources/ims/services/carrier_config/ims", 0755);

    /* The phone's SDP answer copies these lists. Answering a network offer of
       0,2,4,7 (AMR-WB 0,1,2) with only 7 (AMR-WB 2), the stock values, left
       incoming calls with no audio from the phone, and the network ended them
       after about 30 seconds. */
    const char *ir92_cfg = "@ir92\nUsePreconditions:n:0\nSMS_Format::3GPP\namr_modeset_list::0,2,4,7\namrwb_modeset_list::0,1,2\n";
    write_file(IR92_PATH, ir92_cfg, 0644);

    const char *ims_cfg = "@ims\n"
                          "IMS_services:json:[{\"service\":\"voice\",\"feature_tag\":\"urn:urn-7:3gpp-service.ims.icsi.mmtel\",\"network\":[\"LTE\"]}]\n"
                          "Mobility_Management_IMS_Voice_Termination:n:1\n"
                          "IMS_profilesV2:json:[\"IR92\"]\n"
                          "supported_networks:json:[\"LTE\"]\n"
                          "PCSCF_discoveryV2:json:[\"PCO\"]\n"
                          "SDK_Config::3GPP\n"
                          "IMS_always_on:b:true\n"
                          /* ponytail: HD Voice on for every network; add per-carrier
                             handling if one rejects AMR-WB. */
                          "AudioConfig:json:{\"HDvoice\":true}\n"
                          /* REGISTER is about 1900 bytes. Some network routes drop it as
                             fragmented UDP, so send large messages over TCP. */
                          "UDP_max_size:n:1300\n";
    write_file("/var/dca/sources/ims/services/carrier_config/ims/ims", ims_cfg, 0644);

    const char *sources[] = {"ims", "ims_defaults", "omadm", "test"};
    const char *cellular_cfg = "@cellular_protocol_configuration\n"
                               "voice_domain_preference_EUTRAN:n:3\n"
                               "ue_usage_setting::voice_centric\n"
                               "wcdma_to_lte_ps_ho_support:b:false\n";

    char path[512];
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
    {
        snprintf(path, sizeof(path), "/var/dca/sources/%s/services/carrier_config/radio_common", sources[i]);
        mkdir_p(path, 0755);

        snprintf(path, sizeof(path), "/var/dca/sources/%s/services/carrier_config/radio_common/cellular_protocol_configuration", sources[i]);
        write_file(path, cellular_cfg, 0644);
    }

    mkdir_p("/var/dca/sources/test/services/carrier_config/radio_qct", 0755);
    write_file("/var/dca/sources/test/services/carrier_config/radio_qct/ims", "@ims\nIMS_platform::common\n", 0644);
}

/* --- P-CSCF routes --- */

/* ponytail: IPv4 only; extend the character classes if a network gives IPv6 P-CSCFs. */
static int parse_route_failure(const char *line, char *host, char *gateway)
{
    return sscanf(line, "add host %63[0-9.]: gateway %63[0-9.]: Permission denied", host, gateway) == 2;
}

/* ims_rrm can't add its P-CSCF routes without root, and reports each failure in
 * its log. Without them, replies to connections from the P-CSCF leave on the
 * default route, so the phone never receives a BYE sent over TCP. */
static char g_route_gateway[64] = "";

static int address_is_local(const char *address)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (inet_pton(AF_INET, address, &sa.sin_addr) != 1)
        return 1;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;

    int gone = bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno == EADDRNOTAVAIL;
    close(fd);
    return !gone;
}

/* These static routes outlive an IP change, such as airplane mode. ims_rrm reports its
 * routes just before it registers, too late for add_missing_ims_routes() to save that
 * REGISTER, and it doesn't retry the send. So clear them once their address is gone.
 * Only then: ims_rrm may not report them again while its address is unchanged. */
static void delete_stale_ims_routes(void)
{
    char path[512];
    char route_log[512];
    char line[256];

    if (g_route_gateway[0] == '\0' || address_is_local(g_route_gateway))
        return;
    g_route_gateway[0] = '\0';

    snprintf(path, sizeof(path), "%s/ims-start.log", RUN_PATH);
    snprintf(route_log, sizeof(route_log), "%s/route.log", RUN_PATH);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    while (fgets(line, sizeof(line), f))
    {
        char host[64];
        char gateway[64];

        if (!parse_route_failure(line, host, gateway))
            continue;

        char *del_args[] = {"route", "delete", "-host", host, NULL};
        if (run_cmd_spawn(del_args, route_log) == 0)
            log_msg("[BBE] IMS_ROUTE_CLEARED: %s", host);
    }

    fclose(f);
}

static void add_missing_ims_routes(void)
{
    static long offset = 0;
    char path[512];
    char line[256];

    delete_stale_ims_routes();

    snprintf(path, sizeof(path), "%s/ims-start.log", RUN_PATH);
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    /* start_ims() truncates the log. */
    fseek(f, 0, SEEK_END);
    if (ftell(f) < offset)
        offset = 0;
    fseek(f, offset, SEEK_SET);

    while (fgets(line, sizeof(line), f))
    {
        char host[64];
        char gateway[64];

        if (!parse_route_failure(line, host, gateway))
            continue;

        char route_log[512];
        snprintf(route_log, sizeof(route_log), "%s/route.log", RUN_PATH);

        /* These static routes outlive an IP change, such as airplane mode. A
           route through the old address makes every connection to the P-CSCF
           fail with "Can't assign requested address". */
        char *del_args[] = {"route", "delete", "-host", host, NULL};
        char *add_args[] = {"route", "add", "-host", host, gateway, NULL};
        int del_status = run_cmd_spawn(del_args, route_log);
        int add_status = run_cmd_spawn(add_args, route_log);
        log_msg("[BBE] IMS_ROUTE: %s via %s (del %d, add %d)", host, gateway, del_status, add_status);
        snprintf(g_route_gateway, sizeof(g_route_gateway), "%s", gateway);
    }

    offset = ftell(f);
    fclose(f);
}

static int wait_connected(void)
{
    for (int i = 0; i < REGISTER_WAIT_SEC / 2; i++)
    {
        add_missing_ims_routes();
        if (ims_connected_pps())
            return 1;
        if (g_stop_requested)
            return 0;
        if (i % 5 == 0)
        {
            log_msg("[BBE] WAITING:%d", i * 2);
        }
        sleep(2);
    }
    return 0;
}

static int run_berry_check(void)
{
    char icon_buf[4096];
    int desired = ims_connected_pps();
    int actual = (read_file(PPS_ICON, icon_buf, sizeof(icon_buf)) > 0) ? icon_state(icon_buf) : -1;
    printf("[BBE] VoLTE_connected=%d berry_state=%d\n", desired, actual);
    return (desired == actual) ? 0 : 1;
}

/* --- Reconnection Logic with Exponential Backoff --- */

static time_t monotonic_sec(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

static void set_radio_power(const char *state)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "msg::power\nid::1\ndat:json:{\"state\":\"%s\"}\n", state);
    append_file(PPS_RADIO_CONTROL_PATH, cmd);
}

/* A connection rejected on 2G stays stalled after LTE returns, and restarting
 * ims_rrm can't clear it. Cycling the radio does, as airplane mode would. */
static void reset_radio_if_stalled(void)
{
    static time_t last_reset = 0;
    time_t now = monotonic_sec();

    if (!pps_check_attribute_contains(PPS_IMS, "status", "idle") ||
        !pps_check_attribute_contains("/pps/services/phone/public/status", "phone_state", "idle") ||
        (last_reset != 0 && now - last_reset < RADIO_RESET_MIN_INTERVAL_SEC))
    {
        return;
    }

    last_reset = now;
    log_msg("[BBE] RADIO_RESET: IMS connection stalled");
    set_radio_power("OFF");
    /* An ON sent during shutdown is lost, which leaves the phone with no mobile network.
       Shutdown has taken over 6 s. */
    for (int i = 0; i < 30 && !pps_check_attribute_contains(PPS_RADIO_PATH, "power_status", "OFF"); i++)
        sleep(1);
    set_radio_power("ON");

    /* ims_rrm registers by itself once the radio is back. network_technology can't show
       that: it may still read "lte" from before the reset. */
    for (int i = 0; i < 30 && g_lte_monitor_running && !ims_connected_pps(); i++)
        sleep(2);
}

static int reconnect_ims_with_backoff(void)
{
    int attempt = 1;
    int backoff = INITIAL_BACKOFF_SEC;

    while (g_lte_monitor_running && attempt <= MAX_RETRY_ATTEMPTS)
    {
        system_state_t check_st = get_current_system_state();
        if (!is_state_ready(&check_st))
        {
            LOG_VERBOSE("Aborting IMS reconnect sequence: system state lost during retry");
            return 0;
        }

        /* A radio reset or ims_rrm itself may have registered since the last attempt. */
        if (attempt > 1 && find_pid_by_name("ims_rrm", 0) > 0 && ims_connected_pps())
        {
            log_msg("[BBE] CONNECTED");
            return 1;
        }

        LOG_VERBOSE("Attempting IMS reconnect (Attempt %d/%d)...", attempt, MAX_RETRY_ATTEMPTS);
        log_msg("[BBE] RECONNECT_ATTEMPT:%d", attempt);

        stop_ims();
        apply_volte_provisioning();
        delete_stale_ims_routes();
        start_ims();

        if (wait_connected())
        {
            log_msg("[BBE] CONNECTED");
            LOG_VERBOSE("IMS successfully connected on attempt %d", attempt);
            return 1;
        }

        reset_radio_if_stalled();

        log_msg("[BBE] RECONNECT_FAILED_WAITING:%ds", backoff);
        LOG_VERBOSE("Reconnect attempt %d failed. Retrying in %d seconds...", attempt, backoff);

        for (int elapsed = 0; elapsed < backoff && g_lte_monitor_running; elapsed++)
        {
            sleep(1);
        }

        backoff *= 2;
        if (backoff > MAX_BACKOFF_SEC)
        {
            backoff = MAX_BACKOFF_SEC;
        }

        attempt++;
    }

    log_msg("[BBE] RECONNECT_GAVE_UP");
    LOG_VERBOSE("Exceeded maximum reconnect attempts. Retrying in %d seconds.", IMS_RETRY_INTERVAL_SEC);
    return 0;
}

/* --- IMS Health Check --- */

/* ims_rrm is checked too: a crashed process leaves its last PPS status behind.
 * Incoming readiness is not: without an imsconfig file it never becomes ready,
 * and restarting IMS would only drop a working registration. */
static int ims_healthy(void)
{
    return find_pid_by_name("ims_rrm", 0) > 0 && ims_connected_pps();
}

/* LTE, SIM and data can all stay up while IMS dies or loses registration. */
static void check_ims_health(int *was_ready)
{
    static time_t last_check = 0;
    static time_t down_since = 0;
    static int retry_wait = IMS_DOWN_GRACE_SEC;

    if (!is_state_ready(&g_current_state))
    {
        down_since = 0; /* The state-change path owns IMS while conditions are lost. */
        retry_wait = IMS_DOWN_GRACE_SEC;
        return;
    }

    /* Check every pass while recovering, so STATS_OK times the reconnect accurately. */
    time_t now = monotonic_sec();
    if (*was_ready && last_check != 0 && now - last_check < IMS_CHECK_INTERVAL_SEC)
        return;
    last_check = now;

    if (ims_healthy())
    {
        if (!*was_ready && g_drop_pending)
        {
            log_msg("[BBE] CONNECTED");
            stats_on_success();
        }
        *was_ready = 1;
        down_since = 0;
        retry_wait = IMS_DOWN_GRACE_SEC;
    }
    else if (down_since == 0)
    {
        down_since = now;
    }
    else if (now - down_since >= retry_wait)
    {
        if (*was_ready)
        {
            log_msg("[BBE] DROPPED: IMS_LOST");
            stats_on_drop();
        }

        if (reconnect_ims_with_backoff())
        {
            stats_on_success();
            *was_ready = 1;
            down_since = 0;
            retry_wait = IMS_DOWN_GRACE_SEC;
        }
        else
        {
            stats_on_failure();
            *was_ready = 0;
            down_since = monotonic_sec();
            retry_wait = IMS_RETRY_INTERVAL_SEC;
        }
    }
}

/* --- Forced IMS voice (opt-in) --- */

/* ims_rrm refuses IMS calls without this bit. Some networks serve them anyway. */
static unsigned long next_network_service(unsigned long service, int ims_up)
{
    if (ims_up && (service & NET_SERVICE_DATA_LTE))
        return service | NET_SERVICE_IMS_VOICE;

    return service & ~NET_SERVICE_IMS_VOICE;
}

/* cellular-control overwrites the bit whenever it republishes, so this keeps
 * reapplying it. Clearing it when IMS is down lets calls fall back to CS.
 * ponytail: read then write can briefly republish an old mask; the next change
 * from cellular-control corrects it. */
static void sync_voice_flag(int allowed)
{
    static int logged = -1;
    char buf[2048];

    if (!g_force_ims_voice || read_file(PPS_RADIO_PATH, buf, sizeof(buf)) < 0)
        return;

    const char *key = "network_service:n:";
    const char *pos = strstr(buf, key);
    if (!pos)
        return;

    unsigned long service = strtoul(pos + strlen(key), NULL, 10);
    unsigned long next = next_network_service(service, allowed && g_current_state.lte_ok && ims_connected_pps());
    if (next != service)
    {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "[n]network_service:n:%lu\n", next);
        append_file(PPS_RADIO_PATH, cmd);
    }

    int on = (next & NET_SERVICE_IMS_VOICE) != 0;
    if (on != logged)
    {
        log_msg("[BBE] IMS_VOICE_FLAG: %s", on ? "ON" : "OFF");
        logged = on;
    }
}

static int voice_flag_self_test(void)
{
    const struct
    {
        unsigned long service;
        int ims_up;
        unsigned long want;
    } cases[] = {
        {0x20040AUL, 1, 0x220040AUL}, /* LTE, IMS up: set */
        {0x220040AUL, 1, 0x220040AUL}, /* Already set: unchanged */
        {0x220040AUL, 0, 0x20040AUL}, /* IMS down: clear for CS fallback */
        {0x2002UL, 1, 0x2002UL},      /* GSM: never set */
        {0x2002002UL, 1, 0x2002UL}};  /* Left over on GSM: clear */

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        if (next_network_service(cases[i].service, cases[i].ims_up) != cases[i].want)
        {
            fprintf(stderr, "[BBE] voice flag self-test %zu failed\n", i);
            return 1;
        }
    }

    puts("[BBE] Voice flag: 5 cases passed");

    char host[64];
    char gateway[64];
    if (!parse_route_failure("add host 192.0.2.1: gateway 198.51.100.1: Permission denied\n", host, gateway) ||
        strcmp(host, "192.0.2.1") != 0 || strcmp(gateway, "198.51.100.1") != 0 ||
        parse_route_failure("route: writing to routing socket: Permission denied\n", host, gateway) ||
        parse_route_failure("add host fd00::1: gateway fd00::2: Permission denied\n", host, gateway))
    {
        fprintf(stderr, "[BBE] route parser self-test failed\n");
        return 1;
    }

    puts("[BBE] Route parser: 3 cases passed");
    return 0;
}

/* --- Background Multi-PPS Monitor Thread --- */

static void *detailed_pps_monitor_thread(void *arg)
{
    (void)arg;
    struct pollfd pfds[FD_COUNT];
    const char *paths[FD_COUNT] = {PPS_RADIO_PATH, PPS_UICC_PATH, PPS_DATA_PATH};

    for (int i = 0; i < FD_COUNT; i++)
    {
        pfds[i].fd = open(paths[i], O_RDONLY | O_NONBLOCK);
        pfds[i].events = POLLIN | POLLPRI;

        if (pfds[i].fd < 0)
        {
            LOG_VERBOSE("Failed to open PPS: %s", paths[i]);
            for (int j = 0; j < i; j++)
                close(pfds[j].fd);
            return NULL;
        }
    }

    g_current_state = get_current_system_state();
    int was_ready = is_state_ready(&g_current_state) && ims_connected_pps();

    /* No drop will be recorded for this outage, so time recovery from here. */
    if (!was_ready)
        clock_gettime(CLOCK_MONOTONIC, &g_stats.drop_time);

    LOG_VERBOSE("Detailed PPS monitor started. Initial state -> LTE:%d, SIM:%d, Data:%d",
                g_current_state.lte_ok, g_current_state.sim_ok, g_current_state.data_ok);

    char force_path[512];
    snprintf(force_path, sizeof(force_path), "%s/force-ims-voice", g_data_path);
    g_force_ims_voice = access(force_path, F_OK) == 0;

    while (g_lte_monitor_running)
    {
        if (g_reset_stats_requested)
        {
            g_reset_stats_requested = 0;
            stats_reset();
            stats_export_to_file();
        }

        add_missing_ims_routes();
        sync_voice_flag(1);
        check_ims_health(&was_ready);

        int poll_res = poll(pfds, FD_COUNT, 1000);

        if (poll_res < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (poll_res == 0)
            continue;

        for (int i = 0; i < FD_COUNT; i++)
        {
            if (pfds[i].revents & (POLLIN | POLLPRI))
            {
                char buf[1024];
                ssize_t bytes = read(pfds[i].fd, buf, sizeof(buf) - 1);
                (void)bytes;
            }
        }

        system_state_t new_state = get_current_system_state();

        if (new_state.lte_ok != g_current_state.lte_ok ||
            new_state.sim_ok != g_current_state.sim_ok ||
            new_state.data_ok != g_current_state.data_ok)
        {
            log_state_transition(g_current_state, new_state);

            int is_ready = is_state_ready(&new_state);

            if (was_ready && !is_ready)
            {
                stats_on_drop();
                /* A network drop leaves ims_rrm running: it registers again by itself, and
                   check_ims_health() restarts it if not. Stopping it drops its IPsec keys
                   first, so its FIN leaves in cleartext and goes unanswered. That socket
                   then blocks the next REGISTER from the same address with "Address already
                   in use" for up to about 2 minutes. */
                if (!new_state.sim_ok || !new_state.data_ok)
                    stop_ims();
                was_ready = 0;
            }
            else if (!was_ready && is_ready && find_pid_by_name("ims_rrm", 0) <= 0)
            {
                log_msg("[BBE] RESTORED: STARTING_RECONNECT");

                if (reconnect_ims_with_backoff())
                {
                    stats_on_success();
                    was_ready = 1;
                }
                else
                {
                    stats_on_failure();
                    was_ready = 0;
                }
            }

            g_current_state = new_state;
        }
    }

    sync_voice_flag(0);

    for (int i = 0; i < FD_COUNT; i++)
    {
        close(pfds[i].fd);
    }

    return NULL;
}

static void start_lte_monitor(void)
{
    if (!g_lte_monitor_running)
    {
        g_lte_monitor_running = 1;
        pthread_create(&g_lte_thread, NULL, detailed_pps_monitor_thread, NULL);
    }
}

static void stop_lte_monitor(void)
{
    if (g_lte_monitor_running)
    {
        g_lte_monitor_running = 0;
        pthread_join(g_lte_thread, NULL);
    }
}

/* Main thread only: joins the worker threads. */
static void exit_if_stopped(void)
{
    if (g_stop_requested)
    {
        stop_lte_monitor();
        stop_berry();
        exit(0);
    }
}

static int run_until_stopped(void)
{
    while (g_berry_running && !g_stop_requested)
    {
        sleep(1);
    }

    stop_lte_monitor();
    stop_berry();
    return 0;
}

/* --- Signal Handlers & Usage --- */

static void sigusr1_handler(int sig)
{
    (void)sig;
    g_reset_stats_requested = 1;
}

static void signal_handler(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

static void print_usage(const char *prog_name)
{
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -a, --action <check|probe|enable|provision>\n");
    printf("                                      Action to perform (default: check)\n");
    printf("  -r, --refresh                       Force refresh settings during enable\n");
    printf("  -v, --verbose                       Enable verbose logging output\n");
    printf("      --self-test                     Run built-in PPS decoder unit tests\n");
    printf("      --berry-check                   Check current IMS and Berry indicator status\n");
    printf("      --stats                         Print connection statistics\n");
    printf("      --reset-stats                   Reset statistics counters\n");
    printf("  -h, --help                          Show this help message\n");
}

/* A running daemon owns the counters and would overwrite a reset done here. */
static void reset_stats_cli(void)
{
    int daemon_pid = berry_daemon_pid();
    if (daemon_pid > 0 && kill(daemon_pid, SIGUSR1) == 0)
    {
        printf("[BBE] Statistics reset requested from daemon PID %d.\n", daemon_pid);
        return;
    }

    stats_reset();
    stats_export_to_file();
    printf("[BBE] Statistics reset successfully.\n");
}

/* --- Main Entry Point --- */

int main(int argc, char *argv[])
{
    init_slog2_logging("bb10_volte");

    const char *action = "check";
    int force_refresh = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct sigaction sa_usr1;
    memset(&sa_usr1, 0, sizeof(sa_usr1));
    sa_usr1.sa_handler = sigusr1_handler;
    sigemptyset(&sa_usr1.sa_mask);
    sigaction(SIGUSR1, &sa_usr1, NULL);

    static struct option long_options[] = {
        {"action", required_argument, 0, 'a'},
        {"refresh", no_argument, 0, 'r'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {"self-test", no_argument, 0, 1001},
        {"berry-check", no_argument, 0, 1002},
        {"stats", no_argument, 0, 1003},
        {"reset-stats", no_argument, 0, 1004},
        {0, 0, 0, 0}};

    int opt;
    int option_index = 0;

    if (argc > 1 && argv[1][0] != '-')
    {
        action = argv[1];
        if (argc > 2 && (strcasecmp(argv[2], "refresh") == 0))
        {
            force_refresh = 1;
        }
    }
    else
    {
        while ((opt = getopt_long(argc, argv, "a:rvh", long_options, &option_index)) != -1)
        {
            switch (opt)
            {
            case 'a':
                action = optarg;
                break;
            case 'r':
                force_refresh = 1;
                break;
            case 'v':
                g_verbose = 1;
                break;
            case 1001:
                return self_test() || voice_flag_self_test();
            case 1002:
                return run_berry_check();
            case 1003:
                stats_print_cli();
                return 0;
            case 1004:
                reset_stats_cli();
                return 0;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                bbe_exit_fail("INVALID_ARGUMENTS", 1);
            }
        }
    }

    LOG_VERBOSE("Starting application with action='%s', refresh=%d", action, force_refresh);

    init_addon_paths();
    LOG_VERBOSE("Paths initialized: init_path='%s', data_path='%s'", g_init_path, g_data_path);

    setenv("LD_LIBRARY_PATH", "/base/usr/lib:/base/lib:/proc/boot:/base/lib/dll:/base/usr/lib/qt4/lib:/radio/lib:/radio/lib/dll:/radio/usr/lib", 1);
    mkdir_p(RUN_PATH, 0755);

    if (strcasecmp(action, "check") == 0 || strcasecmp(action, "probe") == 0)
    {
        log_msg("[BBE] COMPATIBLE");
        if (strcasecmp(action, "check") == 0)
        {
            ready_check();
            log_msg("[BBE] SIM_LTE_OK");
        }
        log_msg("[BBE] %s", ims_connected_pps() ? "CONNECTED" : "NOT_CONNECTED");
        log_msg("[BBE] %s", berry_daemon_pid() > 0 ? "BERRY_RUNNING" : "BERRY_STOPPED");
        log_msg("[BBE] %s", incoming_ready_pps() ? "INCOMING_READY" : "INCOMING_NOT_READY");
        return 0;
    }

    if (strcasecmp(action, "provision") == 0)
    {
        return refresh_carrier_db(1) == 0 ? 0 : 1;
    }

    if (strcasecmp(action, "enable") != 0)
    {
        bbe_exit_fail("ACTION_INVALID", 1);
    }

    link_carrier_db();

    wait_until_ready();
    LOG_VERBOSE("Ready check passed successfully");

    refresh_carrier_db(0);

    provider_enable();
    LOG_VERBOSE("Provider enabled successfully");

    if (ims_connected_pps() && incoming_ready_pps() && !force_refresh)
    {
        LOG_VERBOSE("IMS and incoming services already connected, starting background monitors");
        apply_carrier_xml();
        start_berry();
        start_lte_monitor();
        log_msg("[BBE] ALREADY_CONNECTED");

        return run_until_stopped();
    }

    log_msg("[BBE] APPLYING");
    write_dca_configs();
    LOG_VERBOSE("DCA configs written");

    apply_volte_provisioning();

    char log_file[512];
    snprintf(log_file, sizeof(log_file), "%s/dca-enable.log", RUN_PATH);

    char *dca_args[] = {
        "dca",
        "-s",
        "-a", "/pps",
        NULL};

    if (run_cmd_spawn(dca_args, log_file) != 0)
    {
        bbe_exit_fail("DCA_FAILED", 1);
    }

    if (!apply_carrier_xml())
    {
        log_msg("[BBE] CARRIER_XML_NOT_FOUND: mcc=%s mnc=%s", g_mcc, g_mnc);
    }
    append_file("/pps/services/escreens/config", "voice_domain_preference_EUTRAN_override::5\n");

    LOG_VERBOSE("Restarting IMS and starting berry monitor thread...");
    stop_ims();
    start_ims();
    start_berry();

    int connected = wait_connected();
    exit_if_stopped();

    /* Without an imsconfig file a restart can't help, and it drops the registration. */
    if (connected && !incoming_ready_pps() && access(g_ims_xml_path, F_OK) == 0)
    {
        LOG_VERBOSE("Incoming state not ready after first attempt, retrying...");
        log_msg("[BBE] INCOMING_RETRY");
        stop_ims();
        apply_volte_provisioning();
        start_ims();
        connected = wait_connected();
        exit_if_stopped();
    }

    if (connected)
    {
        log_msg("[BBE] CONNECTED");
        if (!incoming_ready_pps())
            log_msg("[BBE] INCOMING_NOT_READY");
    }
    else
    {
        log_msg("[BBE] NOT_CONNECTED: monitor will retry");
    }

    /* The monitor also retries when the first registration did not succeed. */
    start_lte_monitor();

    return run_until_stopped();
}