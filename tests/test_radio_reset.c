/* Run with `make test`. Builds ims_reconnect.c against a fake phone and clock. */
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The real header needs QNX's slog2. */
#define LOG_UTILS_H
#define LOG_VERBOSE(...) ((void)0)
void log_msg(const char *fmt, ...);

static time_t g_now = 100000;
static int fake_clock(struct timespec *ts)
{
    ts->tv_sec = g_now;
    ts->tv_nsec = 0;
    return 0;
}
#define clock_gettime(id, ts) fake_clock(ts)
#define sleep(seconds) 0

#include "../src/ims_reconnect.c"

#undef clock_gettime
#undef sleep

volatile int g_lte_monitor_running = 1;
volatile sig_atomic_t g_stop_requested = 0;
system_state_t g_current_state = {1, 1, 1};
int g_drop_pending = 0;

static int g_connected;
static int g_radio_off;
static int g_power_writes;

void log_msg(const char *fmt, ...)
{
    (void)fmt;
}

int pps_check_attribute_contains(const char *pps_path, const char *attr_name, const char *expected_val)
{
    (void)pps_path;
    (void)expected_val;
    if (strcmp(attr_name, "power_status") == 0)
        return g_radio_off;
    return 1; /* IMS and the phone both read idle. */
}

int append_file(const char *path, const char *content)
{
    (void)path;
    g_power_writes++;
    g_radio_off = strstr(content, "OFF") != NULL;
    return 0;
}

int read_file(const char *path, char *buf, size_t max_len)
{
    (void)path;
    (void)buf;
    (void)max_len;
    return -1;
}

int ims_connected_pps(void) { return g_connected; }
int icon_state(char *buf) { (void)buf; return 0; }
int find_pid_by_name(const char *name, int strict) { (void)name; (void)strict; return 1; }
int is_state_ready(const system_state_t *st) { (void)st; return 1; }
system_state_t get_current_system_state(void) { return g_current_state; }
void stop_ims(void) {}
void start_ims(void) {}
void apply_volte_provisioning(void) {}
void delete_stale_ims_routes(void) {}
void add_missing_ims_routes(void) {}
void stats_on_drop(void) {}
void stats_on_success(void) {}
void stats_on_failure(void) {}

/* Moves past the 30 minute spacing, so only the one-per-failure limit can stop a reset. */
static int reset_later(void)
{
    g_now += RADIO_RESET_MIN_INTERVAL_SEC + 1;
    g_power_writes = 0;
    reset_radio_if_stalled();
    return g_power_writes;
}

static void connect_ims(void)
{
    int was_ready = 0;
    g_connected = 1;
    check_ims_health(&was_ready);
    assert(was_ready == 1);
    g_connected = 0;
}

int main(void)
{
    /* A refused IMS bearer: one cycle (off, on), then none however long it fails. */
    assert(reset_later() == 2);
    assert(reset_later() == 0);
    assert(reset_later() == 0);

    /* Connecting makes the next failure eligible again. */
    connect_ims();
    assert(reset_later() == 2);
    assert(reset_later() == 0);

    /* The 30 minute spacing still applies after connecting. */
    connect_ims();
    assert(reset_later() == 2);
    connect_ims();
    g_power_writes = 0;
    reset_radio_if_stalled();
    assert(g_power_writes == 0);

    /* A radio that's already off stays off, and that doesn't use up the reset. */
    g_radio_off = 1;
    assert(reset_later() == 0);
    assert(g_radio_off == 1);
    g_radio_off = 0;
    assert(reset_later() == 2);

    puts("radio reset tests passed");
    return 0;
}
