#ifndef PPS_MONITOR_H
#define PPS_MONITOR_H

#include <signal.h>

extern volatile int g_lte_monitor_running;

/* Set by signal handlers, acted on by the main and monitor threads. */
extern volatile sig_atomic_t g_stop_requested;
extern volatile sig_atomic_t g_reset_stats_requested;

void start_lte_monitor(void);
void exit_if_stopped(void);
int run_until_stopped(void);

#endif /* PPS_MONITOR_H */
