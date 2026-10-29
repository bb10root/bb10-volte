#include "berry_monitor.h"
#include "sys_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sys/pps.h>

#define BERRY_LOCK RUN_PATH "/volte-berry.lock"

static pthread_t g_berry_thread;
volatile sig_atomic_t g_berry_running = 0;

/* Initialize PPS decoder struct and enter root object */
static int pps_decode_init(pps_decoder_t *d, char *buf)
{
    return pps_decoder_initialize(d, buf) == PPS_DECODER_OK &&
           pps_decoder_push(d, NULL) == PPS_DECODER_OK;
}

/* Helper to safely compare a PPS key string value */
static int pps_same_val(pps_decoder_t *d, const char *key, const char *expected)
{
    const char *value = NULL;
    return pps_decoder_get_string(d, key, &value) == PPS_DECODER_OK &&
           value && !strcmp(value, expected);
}

/* Parse raw IMS PPS buffer and check for active VoLTE state */
int parse_ims_pps_buffer(char *buf)
{
    pps_decoder_t d;
    int active = 0;

    if (pps_decode_init(&d, buf) &&
        pps_same_val(&d, "status", "connected") &&
        pps_same_val(&d, "default_ran", "LTE") &&
        pps_decoder_push_array(&d, "deviceConfig") == PPS_DECODER_OK)
    {
        int length = pps_decoder_length(&d);
        for (int i = 0; i < length; ++i)
        {
            if (pps_decoder_goto_index(&d, i) == PPS_DECODER_OK &&
                pps_same_val(&d, NULL, "VoLTE"))
            {
                active = 1;
                break;
            }
        }
    }
    pps_decoder_cleanup(&d);
    return active;
}

/* Check if IMS is connected via PPS file */
int ims_connected_pps(void)
{
    char buf[16384];
    if (read_file(PPS_IMS, buf, sizeof(buf)) < 0)
        return 0;
    return parse_ims_pps_buffer(buf);
}

/* Parse Navigator indicator icon state from PPS buffer */
int icon_state(char *buf)
{
    pps_decoder_t d;
    int state = -1;

    if (pps_decode_init(&d, buf) &&
        pps_decoder_push_object(&d, "dat") == PPS_DECODER_OK &&
        pps_decoder_push_object(&d, "bb_service") == PPS_DECODER_OK)
    {
        if (pps_same_val(&d, "state", "on"))
            state = 1;
        else if (pps_same_val(&d, "state", "off"))
            state = 0;
    }
    pps_decoder_cleanup(&d);
    return state;
}

/* Update Navigator UI indicator state via PPS write */
static int set_icon(int on)
{
    static const char yes[] = "[n]dat:json:{\"bb_service\":{\"state\":\"on\"}}\n";
    static const char no[] = "[n]dat:json:{\"bb_service\":{\"state\":\"off\"}}\n";
    const char *s = on ? yes : no;
    size_t n = sizeof(yes) - 1;

    int fd = open(PPS_ICON, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    ssize_t result = write(fd, s, n);
    close(fd);
    return result == (ssize_t)n ? 0 : -1;
}

/* Restore original BBM / RCP indicator state on shutdown */
static void restore_bbio(void)
{
    char buf[16384];
    pps_decoder_t d;
    int on = 0;

    if (read_file(PPS_BBIO, buf, sizeof(buf)) > 0)
    {
        if (pps_decode_init(&d, buf))
        {
            on = pps_same_val(&d, "rcp_status", "0x03 connected");
            if (!on && pps_same_val(&d, "rcp_status", "0x04 reconnecting"))
            {
                char icon[4096];
                on = read_file(PPS_ICON, icon, sizeof(icon)) > 0 && icon_state(icon) == 1;
            }
        }
        pps_decoder_cleanup(&d);
    }

    if (set_icon(on) != 0)
        log_msg("[BBE] failed to restore BBM indicator");
}

/* Main background monitor thread function using poll() and PPS delta mode */
static void *berry_monitor_thread(void *arg)
{
    (void)arg;
    char delta_buf[4096], icon_buf[4096];
    int desired, actual, previous = -1, lockfd, warned = 0;
    struct flock lock;

    lockfd = open(BERRY_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockfd < 0)
    {
        log_msg("[BBE] indicator lock error");
        return NULL;
    }

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;

    if (fcntl(lockfd, F_SETLK, &lock) < 0)
    {
        log_msg("[BBE] indicator monitor already running in background (lock busy)");
        close(lockfd);
        return NULL;
    }

    g_berry_running = 1;
    log_msg("[BBE] VoLTE berry monitor thread started (PPS ?wait,delta mode)");

    while (g_berry_running)
    {
        int pps_fd = open(PPS_IMS "?wait,delta", O_RDONLY | O_CLOEXEC);
        if (pps_fd < 0)
        {
            if (!warned)
            {
                log_msg("[BBE] waiting for PPS_IMS to become available...");
                warned = 1;
            }
            sleep(1);
            continue;
        }

        warned = 0;
        struct pollfd pfd;
        pfd.fd = pps_fd;
        pfd.events = POLLIN;

        while (g_berry_running)
        {
            ssize_t r = read(pps_fd, delta_buf, sizeof(delta_buf) - 1);
            if (r < 0 && errno != EAGAIN && errno != EINTR)
            {
                log_msg("[BBE] PPS read error (IMS restarted?), reconnecting fd...");
                break;
            }

            desired = ims_connected_pps();
            actual = (read_file(PPS_ICON, icon_buf, sizeof(icon_buf)) > 0) ? icon_state(icon_buf) : -1;

            if (actual != desired)
            {
                if (set_icon(desired) != 0)
                {
                    log_msg("[BBE] could not update Navigator indicator; retrying");
                    sleep(1);
                    continue;
                }
            }

            if (previous != desired)
            {
                log_msg("[BBE] %s", desired ? "IMS VoLTE connected: berry on" : "IMS VoLTE disconnected: berry off");
                previous = desired;
            }

            int poll_ret = poll(&pfd, 1, 5000);
            if (poll_ret < 0)
            {
                if (errno == EINTR)
                    continue;
                log_msg("[BBE] PPS poll error, reconnecting fd...");
                break;
            }

            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
            {
                log_msg("[BBE] PPS connection hung up or invalidated, reconnecting fd...");
                break;
            }
        }

        close(pps_fd);
    }

    restore_bbio();
    log_msg("[BBE] berry monitor stopped; indicator returned to BBM/RCP state");
    close(lockfd);
    return NULL;
}

void start_berry(void)
{
    if (!g_berry_running)
    {
        mkdir_p(RUN_PATH, 0755);
        if (pthread_create(&g_berry_thread, NULL, berry_monitor_thread, NULL) != 0)
        {
            LOG_VERBOSE("Failed to start berry monitor thread");
        }
    }
}

void stop_berry(void)
{
    if (g_berry_running)
    {
        g_berry_running = 0;
        pthread_join(g_berry_thread, NULL);
    }
}

int self_test(void)
{
    const struct
    {
        const char *pps;
        int want;
    } cases[] = {
        {"@status_public\n[n]status::connected\n[n]default_ran::LTE\ndeviceConfig:json:[\"VoLTE\"]\n", 1},
        {"@status_public\nstatus::disconnected\ndefault_ran::LTE\ndeviceConfig:json:[\"VoLTE\"]\n", 0},
        {"@status_public\nstatus::connected\ndefault_ran::WLAN\ndeviceConfig:json:[\"VoLTE\"]\n", 0},
        {"@status_public\nstatus::connected\ndefault_ran::LTE\ndeviceConfig:json:[\"SMS\",\"VoLTE\"]\n", 1},
        {"@status_public\nstatus::connected\ndefault_ran::LTE\ndeviceConfig:json:[\"SMS\"]\n", 0},
        {"@status_public\nstatus::connected\ndefault_ran::LTE\ndeviceConfig:json:[\"VoLTE\"\n", 0},
        {"@status_public\n", 0}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", cases[i].pps);
        int active = parse_ims_pps_buffer(buf);

        if (active != cases[i].want)
        {
            fprintf(stderr, "[BBE] PPS decoder self-test %zu failed\n", i);
            return 1;
        }
    }

    puts("[BBE] PPS decoder: 7 cases passed");
    return 0;
}