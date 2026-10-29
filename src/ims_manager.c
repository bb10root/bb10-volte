#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <getopt.h>
#include <spawn.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/pps.h>
#include <sys/slog2.h>

#include "sys_utils.h"
#include "berry_monitor.h"
#include "ims_carrier_config.h"

#define IR92_PATH "/var/dca/sources/ims/services/carrier_config/ims/ir92"
#define STATS_FILE_PATH "/var/run/bb10_volte_stats.txt"

#define PPS_RADIO_PATH "/pps/services/cellular/radioctrl/status_public"
#define PPS_UICC_PATH "/pps/services/cellular/uicc/card0/status_private"
#define PPS_DATA_PATH "/pps/services/cds/cnice/status_persistent"

#define INITIAL_BACKOFF_SEC 2
#define MAX_BACKOFF_SEC 64
#define MAX_RETRY_ATTEMPTS 6

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
static char g_ims_xml_path[256] = {0};

static pthread_t g_lte_thread;
static volatile int g_lte_monitor_running = 0;
static system_state_t g_current_state = {0};
static connection_stats_t g_stats = {0};

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
        char ch;
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

static void stats_on_drop(void)
{
    g_stats.total_drops++;
    clock_gettime(CLOCK_MONOTONIC, &g_stats.drop_time);
    stats_export_to_file();
}

static void stats_on_success(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

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

static void find_carrier_xml(char *out_path, size_t max_len)
{
    DIR *dir = opendir(g_data_path);
    if (dir)
    {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL)
        {
            if (strncmp(ent->d_name, "carrier-", 8) == 0 && strstr(ent->d_name, ".xml"))
            {
                snprintf(out_path, max_len, "%s/%s", g_data_path, ent->d_name);
                closedir(dir);
                return;
            }
        }
        closedir(dir);
    }
    snprintf(out_path, max_len, "%s/carrier-%s-volte.xml", g_data_path, g_mnc);
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
        sleep(3);
    }

    if (printed_log)
    {
        LOG_VERBOSE("LTE network detected. Proceeding...");
        log_msg("[BBE] LTE_CONNECTED");
    }
}

static void ready_check(void)
{
    if (!pps_check_attribute_contains("/pps/services/phone/public/status", "phone_state", "idle"))
        bbe_exit_fail("CALL_ACTIVE", 1);

    if (!pps_check_attribute_contains(PPS_UICC_PATH, "uicc_state", "READY"))
        bbe_exit_fail("SIM_NOT_READY", 1);

    if (!detect_sim_carrier())
        bbe_exit_fail("SIM_UNSUPPORTED", 1);

    wait_for_lte();

    if (!pps_check_attribute_contains(PPS_DATA_PATH, "data_service_mode", "ON"))
        bbe_exit_fail("DATA_OFF", 1);
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

    pid_t pid;
    int status = posix_spawnp(&pid, "on", &actions, &attr, args, NULL);

    if (status != 0)
    {
        LOG_VERBOSE("Failed to spawn ims_rrm process, error: %d", status);
    }
    else
    {
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
        if (ims_resolve_config_path(g_mcc, g_mnc, g_ims_xml_path, sizeof(g_ims_xml_path)) != 0)
        {
            snprintf(g_ims_xml_path, sizeof(g_ims_xml_path), "/var/ims/carrier/imsconfig_0%s.xml", g_mnc);
        }
    }

    ims_enable_volte_provisioning(g_ims_xml_path);
}

static void write_dca_configs(void)
{
    mkdir_p("/var/dca/sources/ims/services/carrier_config/ims", 0755);

    const char *ir92_cfg = "@ir92\nUsePreconditions:n:0\nSMS_Format::3GPP\namr_modeset_list::7\namrwb_modeset_list::2\n";
    write_file(IR92_PATH, ir92_cfg, 0644);

    const char *ims_cfg = "@ims\n"
                          "IMS_services:json:[{\"service\":\"voice\",\"feature_tag\":\"urn:urn-7:3gpp-service.ims.icsi.mmtel\",\"network\":[\"LTE\"]}]\n"
                          "Mobility_Management_IMS_Voice_Termination:n:1\n"
                          "IMS_profilesV2:json:[\"IR92\"]\n"
                          "supported_networks:json:[\"LTE\"]\n"
                          "PCSCF_discoveryV2:json:[\"PCO\"]\n"
                          "SDK_Config::3GPP\n"
                          "IMS_always_on:b:true\n";
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

static int wait_connected(void)
{
    for (int i = 0; i < 30; i++)
    {
        if (ims_connected_pps())
            return 1;
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

        LOG_VERBOSE("Attempting IMS reconnect (Attempt %d/%d)...", attempt, MAX_RETRY_ATTEMPTS);
        log_msg("[BBE] RECONNECT_ATTEMPT:%d", attempt);

        stop_ims();
        apply_volte_provisioning();
        start_ims();

        if (wait_connected() && incoming_ready_pps())
        {
            log_msg("[BBE] CONNECTED");
            LOG_VERBOSE("IMS successfully connected on attempt %d", attempt);
            return 1;
        }

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
    LOG_VERBOSE("Exceeded maximum reconnect attempts. Waiting for next system state event.");
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
    int was_ready = is_state_ready(&g_current_state);

    LOG_VERBOSE("Detailed PPS monitor started. Initial state -> LTE:%d, SIM:%d, Data:%d",
                g_current_state.lte_ok, g_current_state.sim_ok, g_current_state.data_ok);

    while (g_lte_monitor_running)
    {
        int poll_res = poll(pfds, FD_COUNT, 1000);

        if (poll_res < 0)
            break;
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
                stop_ims();
                was_ready = 0;
            }
            else if (!was_ready && is_ready)
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

/* --- Signal Handlers & Usage --- */

static void sigusr1_handler(int sig)
{
    (void)sig;
    stats_reset();
    stats_export_to_file();
}

static void signal_handler(int sig)
{
    (void)sig;
    stop_lte_monitor();
    stop_berry();
    exit(0);
}

static void print_usage(const char *prog_name)
{
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -a, --action <check|probe|enable>   Action to perform (default: check)\n");
    printf("  -r, --refresh                       Force refresh settings during enable\n");
    printf("  -v, --verbose                       Enable verbose logging output\n");
    printf("      --self-test                     Run built-in PPS decoder unit tests\n");
    printf("      --berry-check                   Check current IMS and Berry indicator status\n");
    printf("      --stats                         Print connection statistics\n");
    printf("      --reset-stats                   Reset statistics counters\n");
    printf("  -h, --help                          Show this help message\n");
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
                return self_test();
            case 1002:
                return run_berry_check();
            case 1003:
                stats_print_cli();
                return 0;
            case 1004:
                stats_reset();
                stats_export_to_file();
                printf("[BBE] Statistics reset successfully.\n");
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
        log_msg("[BBE] %s", g_berry_running ? "BERRY_RUNNING" : "BERRY_STOPPED");
        log_msg("[BBE] %s", incoming_ready_pps() ? "INCOMING_READY" : "INCOMING_NOT_READY");
        return 0;
    }

    if (strcasecmp(action, "enable") != 0)
    {
        bbe_exit_fail("ACTION_INVALID", 1);
    }

    ready_check();
    LOG_VERBOSE("Ready check passed successfully");

    provider_enable();
    LOG_VERBOSE("Provider enabled successfully");

    if (ims_connected_pps() && incoming_ready_pps() && !force_refresh)
    {
        LOG_VERBOSE("IMS and incoming services already connected, starting background monitors");
        start_berry();
        start_lte_monitor();
        log_msg("[BBE] ALREADY_CONNECTED");

        while (g_berry_running)
        {
            sleep(1);
        }

        stop_lte_monitor();
        return 0;
    }

    log_msg("[BBE] APPLYING");
    write_dca_configs();
    LOG_VERBOSE("DCA configs written");

    apply_volte_provisioning();
    LOG_VERBOSE("VoLTE provisioning applied via XML");

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

    char carrier_xml[512];
    find_carrier_xml(carrier_xml, sizeof(carrier_xml));
    chmod(carrier_xml, 0644);
    LOG_VERBOSE("Carrier XML resolved to: %s", carrier_xml);

    char cmd_buf[512];
    snprintf(cmd_buf, sizeof(cmd_buf), "cdsc_xml::%s\n", carrier_xml);
    append_file("/pps/services/cds/cnice/status_persistent", cmd_buf);
    append_file("/pps/services/escreens/config", "voice_domain_preference_EUTRAN_override::5\n");

    LOG_VERBOSE("Restarting IMS and starting berry monitor thread...");
    stop_ims();
    start_ims();
    start_berry();

    if (!wait_connected())
        bbe_exit_fail("REGISTER_TIMEOUT", 1);

    if (!incoming_ready_pps())
    {
        LOG_VERBOSE("Incoming state not ready after first attempt, retrying...");
        log_msg("[BBE] INCOMING_RETRY");
        stop_ims();
        apply_volte_provisioning();
        start_ims();
        if (!wait_connected())
            bbe_exit_fail("REGISTER_TIMEOUT", 1);
    }

    if (!incoming_ready_pps())
        bbe_exit_fail("INCOMING_FAILED", 1);

    log_msg("[BBE] CONNECTED");

    /* Start background monitoring thread after initial successful connection */
    start_lte_monitor();

    while (g_berry_running)
    {
        sleep(1);
    }

    stop_lte_monitor();
    return 0;
}