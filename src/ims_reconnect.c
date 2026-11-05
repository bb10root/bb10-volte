#include "ims_reconnect.h"
#include "sys_utils.h"
#include "berry_monitor.h"
#include "daemon_stats.h"
#include "ims_control.h"
#include "ims_routes.h"
#include "pps_monitor.h"
#include "system_state.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define INITIAL_BACKOFF_SEC 2
#define MAX_BACKOFF_SEC 64
#define MAX_RETRY_ATTEMPTS 6

#define IMS_CHECK_INTERVAL_SEC 10
#define IMS_DOWN_GRACE_SEC 60     /* Tolerated re-registration time before restarting IMS */
#define IMS_RETRY_INTERVAL_SEC 300 /* Wait after a reconnect sequence gives up */
#define REGISTER_WAIT_SEC 60
#define RADIO_RESET_MIN_INTERVAL_SEC 1800

int wait_connected(void)
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

int run_berry_check(void)
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

int reconnect_ims_with_backoff(void)
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
void check_ims_health(int *was_ready)
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
