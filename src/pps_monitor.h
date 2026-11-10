#ifndef PPS_MONITOR_H
#define PPS_MONITOR_H

#include <signal.h>

extern volatile int g_lte_monitor_running;

/* Set by signal handlers, acted on by the main and monitor threads. */
extern volatile sig_atomic_t g_stop_requested;
extern volatile sig_atomic_t g_restart_requested;
extern volatile sig_atomic_t g_reset_stats_requested;

void start_lte_monitor(void);
void exit_if_stopped(void);
int run_until_stopped(void);

/* Defined in ims_manager.c, next to the signal handler. */
void log_stop_sender(void);

#endif /* PPS_MONITOR_H */
