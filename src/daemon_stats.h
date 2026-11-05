#ifndef DAEMON_STATS_H
#define DAEMON_STATS_H

#include <time.h>

typedef struct
{
    unsigned int total_drops;
    unsigned int successful_reconnects;
    unsigned int failed_reconnects;
    double total_recovery_time_sec;
    struct timespec drop_time;
    time_t last_reset_time;
} connection_stats_t;

extern connection_stats_t g_stats;
extern int g_drop_pending;

void stats_reset(void);
void stats_export_to_file(void);
void stats_print_cli(void);
void stats_on_drop(void);
void stats_on_success(void);
void stats_on_failure(void);

#endif /* DAEMON_STATS_H */
