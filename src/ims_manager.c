#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

#include "sys_utils.h"
#include "berry_monitor.h"
#include "carrier_apply.h"
#include "daemon_stats.h"
#include "ims_control.h"
#include "ims_reconnect.h"
#include "pps_monitor.h"
#include "self_test.h"
#include "system_state.h"

/* Set by signal handlers, acted on by the main and monitor threads. */
volatile sig_atomic_t g_stop_requested = 0;
volatile sig_atomic_t g_reset_stats_requested = 0;

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
