#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <devctl.h>
#include <spawn.h>
#include <sys/procfs.h>

#include "sys_utils.h"
#include "berry_monitor.h"
#include "carrier_apply.h"
#include "daemon_stats.h"
#include "ims_control.h"
#include "ims_reconnect.h"
#include "pps_monitor.h"
#include "self_test.h"
#include "system_state.h"
#include "volte_settings.h"

/* Set by signal handlers, acted on by the main and monitor threads. */
volatile sig_atomic_t g_stop_requested = 0;
volatile sig_atomic_t g_restart_requested = 0;
volatile sig_atomic_t g_reset_stats_requested = 0;

/* --- Signal Handlers & Usage --- */

static void sigusr1_handler(int sig)
{
    (void)sig;
    g_reset_stats_requested = 1;
}

static void sighup_handler(int sig)
{
    (void)sig;
    g_restart_requested = 1;
}

static volatile sig_atomic_t g_stop_signal = 0;
static volatile sig_atomic_t g_stop_sender = 0;
static char g_stop_sender_exe[256];

/* The sender may be a short-lived kill or script, so its name is read here, not when logging. */
static void signal_handler(int sig, siginfo_t *info, void *context)
{
    (void)context;

    if (!g_stop_requested)
    {
        int saved_errno = errno;

        g_stop_signal = sig;
        g_stop_sender = info->si_pid;
        exe_name_signal_safe(info->si_pid, g_stop_sender_exe, sizeof(g_stop_sender_exe));
        errno = saved_errno;
    }
    g_stop_requested = 1;
}

void log_stop_sender(void)
{
    log_msg("[BBE] STOP_SIGNAL: sig=%d from pid=%d exe=%s", (int)g_stop_signal, (int)g_stop_sender, g_stop_sender_exe);
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

/* The system ends every process carrying an app's id when that app exits, and a daemon started
   from an app's installer inherits it. Appid 1 (boot) or our own pid is fine; anything else,
   start over as a new application. Only for the daemon: a one-shot command must keep its caller. */
static void become_own_app(char *argv[])
{
    debug_process_t info;
    int pfd = open("/proc/self/as", O_RDONLY);
    int got = pfd >= 0 && devctl(pfd, DCMD_PROC_INFO, &info, sizeof info, NULL) == EOK;
    if (pfd >= 0)
        close(pfd);
    if (!got || info.appid == 1 || info.appid == (unsigned)getpid())
        return;

    posix_spawnattr_t attr;
    pid_t child;
    if (posix_spawnattr_init(&attr) != 0)
        return;
    int rc = posix_spawnattr_setxflags(&attr, POSIX_SPAWN_NEWAPP | POSIX_SPAWN_SETSID | POSIX_SPAWN_NOZOMBIE);
    if (rc == 0)
        rc = posix_spawn(&child, argv[0], NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc == 0)
        _exit(0);
    log_msg("[BBE] NEWAPP_FAILED: %s", strerror(rc));
}

/* --- Main Entry Point --- */

int main(int argc, char *argv[])
{
    /* Whoever started us may have leaked its descriptors (an app's navigator, GPU and screen
       connections did): the system ends every process holding them when that app exits, and
       ims_rrm would inherit them from us too. Keep only stdin, stdout and stderr. */
    for (long fd = 3, max = sysconf(_SC_OPEN_MAX); fd < max; fd++)
        close((int)fd);

    init_slog2_logging("bb10_volte");

    const char *action = "check";
    int force_refresh = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct sigaction sa_usr1;
    memset(&sa_usr1, 0, sizeof(sa_usr1));
    sa_usr1.sa_handler = sigusr1_handler;
    sigemptyset(&sa_usr1.sa_mask);
    sigaction(SIGUSR1, &sa_usr1, NULL);

    struct sigaction sa_hup;
    memset(&sa_hup, 0, sizeof(sa_hup));
    sa_hup.sa_handler = sighup_handler;
    sigemptyset(&sa_hup.sa_mask);
    sigaction(SIGHUP, &sa_hup, NULL);

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
        return refresh_carrier_db(1) >= 0 ? 0 : 1;
    }

    if (strcasecmp(action, "enable") != 0)
    {
        bbe_exit_fail("ACTION_INVALID", 1);
    }
    become_own_app(argv);

    link_carrier_db();

    wait_until_ready();
    LOG_VERBOSE("Ready check passed successfully");

    refresh_carrier_db(0);

    provider_enable();
    LOG_VERBOSE("Provider enabled successfully");

    /* Load the dial mode for this carrier and write the settings app's status file. */
    volte_settings_init();

    /* VoLTE is off, by the user's choice or because every dial format was refused on this
       network: leave ims_rrm stopped, even one still registered from before, so calls use 3G or
       2G. The monitor thread stays, since it polls the settings app's requests (turning VoLTE
       back on). No berry monitor: it follows IMS. */
    if (volte_settings_voice_off())
    {
        if (strcmp(volte_settings_selection(), "off") == 0)
            log_msg("[BBE] VOLTE_OFF: turned off in the settings app; calls use 3G or 2G");
        else
            log_msg("[BBE] VOLTE_OFF: network refused every dial format; calls use 3G or 2G");
        write_dca_configs();
        if (push_dca_configs() != 0)
            log_msg("[BBE] DCA_FAILED: VoLTE off");
        /* The carrier profile stays, so turning VoLTE back on has what ims_rrm needs. */
        apply_carrier_xml();
        append_file("/pps/services/escreens/config", "voice_domain_preference_EUTRAN_override::5\n");
        stop_ims();
        start_lte_monitor();
        return run_until_stopped(0);
    }

    if (ims_connected_pps() && incoming_ready_pps() && !force_refresh)
    {
        LOG_VERBOSE("IMS and incoming services already connected, starting background monitors");
        apply_carrier_xml();
        start_berry();
        start_lte_monitor();
        log_msg("[BBE] ALREADY_CONNECTED");

        return run_until_stopped(1);
    }

    log_msg("[BBE] APPLYING");
    write_dca_configs();
    LOG_VERBOSE("DCA configs written");

    apply_volte_provisioning();

    if (push_dca_configs() != 0)
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

    return run_until_stopped(1);
}
