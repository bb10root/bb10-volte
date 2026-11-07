#include "pps_monitor.h"
#include "sys_utils.h"
#include "berry_monitor.h"
#include "daemon_stats.h"
#include "ims_control.h"
#include "ims_reconnect.h"
#include "ims_routes.h"
#include "system_state.h"
#include "voice_flag.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

enum
{
    FD_RADIO = 0,
    FD_UICC,
    FD_DATA,
    FD_COUNT
};

static pthread_t g_lte_thread;
volatile int g_lte_monitor_running = 0;

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

void start_lte_monitor(void)
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
void exit_if_stopped(void)
{
    if (g_stop_requested)
    {
        log_stop_sender();
        stop_lte_monitor();
        stop_berry();
        exit(0);
    }
}

int run_until_stopped(void)
{
    while (g_berry_running && !g_stop_requested)
    {
        sleep(1);
    }

    if (g_stop_requested)
        log_stop_sender();
    stop_lte_monitor();
    stop_berry();
    return 0;
}
