#include "daemon_stats.h"
#include "sys_utils.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/slog2.h>

#define STATS_FILE_PATH "/var/run/bb10_volte_stats.txt"

connection_stats_t g_stats = {0};

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

int g_drop_pending = 0;

void stats_on_drop(void)
{
    g_drop_pending = 1;
    g_stats.total_drops++;
    clock_gettime(CLOCK_MONOTONIC, &g_stats.drop_time);
    stats_export_to_file();
}

void stats_on_success(void)
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

void stats_on_failure(void)
{
    g_stats.failed_reconnects++;

    slog2f(NULL, 0, SLOG2_WARNING,
           "[STATS] IMS Reconnect Failed | Drops: %u | Reconnects: %u | Failed: %u",
           g_stats.total_drops,
           g_stats.successful_reconnects,
           g_stats.failed_reconnects);

    stats_export_to_file();
}
