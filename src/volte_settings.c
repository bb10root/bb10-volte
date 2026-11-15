#include "volte_settings.h"
#include "volte_settings_parse.h"
#include "berry_monitor.h"
#include "carrier_profile.h"
#include "proc_utils.h"
#include "ims_control.h"
#include "ims_carrier_config.h"
#include "carrier_lookup.h"
#include "ims_reconnect.h"
#include "carrier_apply.h"
#include "pps_monitor.h"
#include "system_state.h"
#include "sys_utils.h"
#include "zip_writer.h"
#include "ims_patch.h"
#include "voice_flag.h"
#include "log_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DOCS_DIR "/accounts/1000/shared/documents"
#define REQUEST_PATH DOCS_DIR "/volte-domain.request"
#define STATUS_PATH DOCS_DIR "/volte-domain.status"
#define PHONE_STATUS "/pps/services/phone/public/status"
#define DEBUG_OUT "/var/tmp/volte-debug-bundle"
#define LOG_MAX (768u * 1024u)

static char g_mode[16] = "tel";
static char g_learned[128];
static char g_imsi[64];
static volte_call_t g_call = {"no call seen"};
static char g_last_request[32];
static char g_debug_state[16] = "idle";
static char g_debug_file[64];
static char g_message[160];
static char g_selection[8] = "auto";     /* "auto", "manual" or "off" (VoLTE turned off by the user) */
static char g_failed[4];                 /* letters of modes refused while automatic */
static char g_auto_state[12] = "testing"; /* "testing", "confirmed", "off" */
static char g_auto_note[160];            /* last automatic event, memory only */
static unsigned long g_off_sim;          /* hash of the SIM VoLTE was turned off with, 0 unknown */
static unsigned long g_last_call_id;     /* last outgoing call already evaluated */
static volatile int g_phone_changed;
static int g_carrier_id = -1;
static int g_inited = 0;

/* On/off knobs the settings app can flip. Each is a flag file in the data folder: present = on.
   A per-carrier knob's file is <name>-<carrier id>. A file shipped in pkg/data comes back
   on a reinstall. */
typedef struct
{
    const char *name;
    int per_carrier;
} knob_t;

static const knob_t g_knobs[] = {
    {"force-ims-voice", 0}, /* keep the VoPS bit set while IMS is registered (voice_flag.c) */
    {"rtcp-bw", 1},         /* non-zero SDP b=RS/b=RR, ims_patch.c PATCH_RTCP */
};
#define KNOB_COUNT (sizeof(g_knobs) / sizeof(g_knobs[0]))

static int knob_path(const knob_t *k, char *out, size_t n)
{
    int len;

    if (k->per_carrier && g_carrier_id <= 0)
        return -1;
    if (k->per_carrier)
        len = snprintf(out, n, "%s/%s-%d", g_data_path, k->name, g_carrier_id);
    else
        len = snprintf(out, n, "%s/%s", g_data_path, k->name);
    return len < 0 || (size_t)len >= n ? -1 : 0;
}

static int knob_on(const knob_t *k)
{
    char path[512];

    return knob_path(k, path, sizeof(path)) == 0 && access(path, F_OK) == 0;
}

static const knob_t *knob_find(const char *name)
{
    size_t i;

    for (i = 0; i < KNOB_COUNT; i++)
    {
        if (strcmp(g_knobs[i].name, name) == 0)
            return &g_knobs[i];
    }
    return NULL;
}

/* --- per-carrier data files (dial-modes / learned-domains) --- */

static int read_kv(const char *path, int id, char *out, size_t out_len)
{
    char buf[16384];
    char *line, *save = NULL;
    int found = 0;

    if (read_file(path, buf, sizeof(buf)) < 0)
        return -1;

    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        char *sp;
        size_t n;

        if (atoi(line) != id)
            continue;
        sp = strpbrk(line, " \t");
        if (!sp)
            continue;
        while (*sp == ' ' || *sp == '\t')
            sp++;
        n = strcspn(sp, " \t\r\n");
        if (n == 0 || n >= out_len)
            continue;
        memcpy(out, sp, n);
        out[n] = '\0';
        found = 1;
    }
    return found ? 0 : -1;
}

static int write_kv(const char *path, int id, const char *value)
{
    char in[16384];
    char out[16384];
    size_t used = 0;

    in[0] = '\0';
    read_file(path, in, sizeof(in));

    char *line, *save = NULL;
    for (line = strtok_r(in, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    {
        if (atoi(line) == id)
            continue;
        int n = snprintf(out + used, sizeof(out) - used, "%s\n", line);
        if (n < 0 || (size_t)n >= sizeof(out) - used)
            return -1;
        used += (size_t)n;
    }
    int n = snprintf(out + used, sizeof(out) - used, "%d %s\n", id, value);
    if (n < 0 || (size_t)n >= sizeof(out) - used)
        return -1;
    used += (size_t)n;
    return write_file(path, out, 0644);
}

/* --- automatic selection state, <carrier_id> <token> in dial-auto ---
   Tokens: manual | forced (VoLTE turned off by the user) | ok (confirmed) |
   off or off-<SIM hash> (VoLTE off, once "done") | auto | auto-<failed letters> (testing). */

static void save_auto(void)
{
    char path[512], tok[16];

    if (g_carrier_id <= 0)
        return;
    if (strcmp(g_selection, "off") == 0)
        snprintf(tok, sizeof(tok), "forced");
    else if (strcmp(g_selection, "manual") == 0)
        snprintf(tok, sizeof(tok), "manual");
    else if (strcmp(g_auto_state, "confirmed") == 0)
        snprintf(tok, sizeof(tok), "ok");
    else if (strcmp(g_auto_state, "off") == 0 && g_off_sim)
        snprintf(tok, sizeof(tok), "off-%08lx", g_off_sim);
    else if (strcmp(g_auto_state, "off") == 0)
        snprintf(tok, sizeof(tok), "off");
    else if (g_failed[0])
        snprintf(tok, sizeof(tok), "auto-%s", g_failed);
    else
        snprintf(tok, sizeof(tok), "auto");
    snprintf(path, sizeof(path), "%s/dial-auto", g_data_path);
    write_kv(path, g_carrier_id, tok);
}

/* A missing or unknown token means automatic, nothing tried yet. */
static void load_auto(const char *tok)
{
    snprintf(g_selection, sizeof(g_selection), "auto");
    snprintf(g_auto_state, sizeof(g_auto_state), "testing");
    g_failed[0] = '\0';
    g_off_sim = 0;

    if (strcmp(tok, "manual") == 0)
        snprintf(g_selection, sizeof(g_selection), "manual");
    else if (strcmp(tok, "forced") == 0)
        snprintf(g_selection, sizeof(g_selection), "off");
    else if (strcmp(tok, "ok") == 0)
        snprintf(g_auto_state, sizeof(g_auto_state), "confirmed");
    else if (strcmp(tok, "off") == 0 || strcmp(tok, "done") == 0)
        snprintf(g_auto_state, sizeof(g_auto_state), "off");
    else if (strncmp(tok, "off-", 4) == 0)
    {
        snprintf(g_auto_state, sizeof(g_auto_state), "off");
        g_off_sim = strtoul(tok + 4, NULL, 16);
    }
    else if (strncmp(tok, "auto-", 5) == 0)
        snprintf(g_failed, sizeof(g_failed), "%s", tok + 5);
}

/* --- status file --- */

/* APNs of the ims and internet services in the carrier config cellular-control reads (a link to
   the addon's profile, or the stock file). Both are empty when it cannot be read. */
static void read_apns(char *ims, char *internet, size_t len)
{
    char path[128];
    char *xml;

    ims[0] = internet[0] = '\0';
    if (g_carrier_id <= 0 || !(xml = malloc(64 * 1024)))
        return;
    snprintf(path, sizeof(path), CARRIER_CONFIG_DIR "/%d.xml", g_carrier_id);
    if (read_file(path, xml, 64 * 1024) >= 0)
    {
        volte_settings_parse_apn(xml, "ims", ims, len);
        volte_settings_parse_apn(xml, "internet", internet, len);
    }
    free(xml);
}

static void write_status(const char *state, const char *message)
{
    char buf[4096];
    char knobs[128];
    char ims_apn[128], internet_apn[128];
    size_t i, used = 0;

    if (message)
        snprintf(g_message, sizeof(g_message), "%s", message);

    /* name:0|1 for every knob that can be flipped now; a per-carrier one waits for the carrier. */
    knobs[0] = '\0';
    for (i = 0; i < KNOB_COUNT; i++)
    {
        char path[512];
        int n;

        if (knob_path(&g_knobs[i], path, sizeof(path)) != 0)
            continue;
        n = snprintf(knobs + used, sizeof(knobs) - used, "%s%s:%d", used ? "," : "",
                     g_knobs[i].name, knob_on(&g_knobs[i]));
        if (n < 0 || (size_t)n >= sizeof(knobs) - used)
        {
            knobs[used] = '\0';
            break;
        }
        used += (size_t)n;
    }
    read_apns(ims_apn, internet_apn, sizeof(ims_apn));
    snprintf(buf, sizeof(buf),
             "request=%s\n"
             "carrier=%d\n"
             "mode=%s\n"
             "state=%s\n"
             "imsi_domain=%s\n"
             "learned_domain=%s\n"
             "last_result=%s\n"
             "last_uri_domain=%s\n"
             "debug_state=%s\n"
             "debug_file=%s\n"
             "message=%s\n"
             "selection=%s\n"
             "auto_state=%s\n"
             "auto_note=%s\n"
             "knobs=%s\n"
             "carrier_list=%s\n"
             "mobile_data=%s\n"
             "ims=%s\n"
             "ims_rrm=%s\n"
             "mcc=%s\n"
             "mnc=%s\n"
             "ims_apn=%s\n"
             "internet_apn=%s\n"
             "updated=%ld\n",
             g_last_request, g_carrier_id, g_mode, state, g_imsi, g_learned,
             g_call.result, g_call.uri, g_debug_state, g_debug_file, g_message,
             g_selection, g_auto_state, g_auto_note, knobs,
             carrier_db_present() ? "ok" : "missing",
             pps_check_attribute_contains(PPS_DATA_PATH, "data_service_mode", "ON") ? "on" : "off",
             ims_connected_pps() ? "connected" : "disconnected",
             find_pid_by_name("ims_rrm", 0) > 0 ? "running" : "stopped",
             g_mcc, g_mnc, ims_apn, internet_apn, (long)time(NULL));
    write_file(STATUS_PATH, buf, 0666);
}

static int call_active(void)
{
    char buf[2048];
    if (read_file(PHONE_STATUS, buf, sizeof(buf)) < 0)
        return 0;
    return strstr(buf, "phone_active:b:true") != NULL;
}

/* --- sip log, read only when the app asks --- */

static void scan_log(void)
{
    char *buf = malloc(LOG_MAX);
    const char *cmd;
    int n;

    if (!buf)
        return;

    cmd = access("/usr/bin/slog2info", X_OK) == 0 ? "/usr/bin/slog2info -b ims_rrm"
                                                  : "slog2info -b ims_rrm";
    n = run_cmd_capture(cmd, buf, LOG_MAX);
    if (n > 0)
    {
        char domain[128];

        if (volte_settings_parse_learned_domain(buf, domain, sizeof(domain)))
        {
            if (strcmp(domain, g_learned) != 0)
            {
                snprintf(g_learned, sizeof(g_learned), "%s", domain);
                if (g_carrier_id > 0)
                {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/learned-domains", g_data_path);
                    write_kv(path, g_carrier_id, domain);
                }
            }
        }
        volte_settings_parse_last_call(buf, &g_call);
    }
    free(buf);
}

/* --- debug bundle --- */

static void run_debug(void)
{
    char zip[512];
    char name[64];
    char cmd[1024];
    char date[16];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    int rc;

    if (tm)
        strftime(date, sizeof(date), "%Y%m%d", tm);
    else
        snprintf(date, sizeof(date), "00000000");
    snprintf(name, sizeof(name), "volte-debug-%s.zip", date);
    snprintf(zip, sizeof(zip), "%s/%s", DOCS_DIR, name);

    snprintf(g_debug_state, sizeof(g_debug_state), "running");
    g_debug_file[0] = '\0';
    write_status("applying", "Collecting debug log...");

    char *rm_argv[] = {"rm", "-rf", DEBUG_OUT, NULL};
    run_cmd_spawn(rm_argv, NULL);
    mkdir_p(DEBUG_OUT, 0755);

    snprintf(cmd, sizeof(cmd),
             "PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin; export PATH; "
             "cat \"%s/volte-debug.sh\" | sh -s \"%s\"", g_data_path, DEBUG_OUT);
    char *sh_argv[] = {"/bin/sh", "-c", cmd, NULL};
    rc = run_cmd_spawn(sh_argv, NULL);
    if (rc == 0)
        rc = zip_directory(DEBUG_OUT, zip);

    if (rc == 0)
    {
        snprintf(g_debug_state, sizeof(g_debug_state), "done");
        snprintf(g_debug_file, sizeof(g_debug_file), "%s", name);
        char msg[160];
        snprintf(msg, sizeof(msg), "Debug log saved: %s", name);
        write_status("idle", msg);
        run_cmd_spawn(rm_argv, NULL);
    }
    else
    {
        snprintf(g_debug_state, sizeof(g_debug_state), "error");
        write_status("error", "Debug log failed");
    }
}

/* --- public --- */

void volte_settings_init(void)
{
    char tok[16];

    g_carrier_id = ims_get_carrier_id(g_mcc, g_mnc, g_gid1);
    snprintf(g_imsi, sizeof(g_imsi), "ims.mnc%03d.mcc%s.3gppnetwork.org", atoi(g_mnc), g_mcc);

    g_mode[0] = '\0';
    g_learned[0] = '\0';
    if (g_carrier_id > 0)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/dial-modes", g_data_path);
        read_kv(path, g_carrier_id, g_mode, sizeof(g_mode));
        snprintf(path, sizeof(path), "%s/learned-domains", g_data_path);
        read_kv(path, g_carrier_id, g_learned, sizeof(g_learned));
        snprintf(path, sizeof(path), "%s/dial-auto", g_data_path);
        tok[0] = '\0';
        read_kv(path, g_carrier_id, tok, sizeof(tok));
        load_auto(tok);

        /* Another SIM of the same carrier may be on a contract that allows VoLTE. */
        if (g_sim_id && strcmp(g_selection, "auto") == 0 && strcmp(g_auto_state, "off") == 0)
        {
            if (g_off_sim == 0)
            {
                g_off_sim = g_sim_id;
                save_auto();
            }
            else if (g_off_sim != g_sim_id)
            {
                g_failed[0] = '\0';
                g_off_sim = 0;
                snprintf(g_auto_state, sizeof(g_auto_state), "testing");
                snprintf(g_auto_note, sizeof(g_auto_note), "New SIM: trying VoLTE again");
                save_auto();
                log_msg("[BBE] DIAL_AUTO: new SIM, VoLTE back on");
            }
        }
    }

    if (g_mode[0] == '\0')
    {
        char domain[128];
        if (g_carrier_id > 0 &&
            ims_lookup_home_domain(g_data_path, g_carrier_id, domain, sizeof(domain)) == 0)
        {
            snprintf(g_mode, sizeof(g_mode), "sip-learned");
            snprintf(g_learned, sizeof(g_learned), "%s", domain);
        }
        else
        {
            snprintf(g_mode, sizeof(g_mode), "tel");
        }
    }

    /* The IMSI-derived domain is always known from the SIM: use it as the initial learned
       domain so the toggle is available on carriers whose IMPU lives in 3gppnetwork.org.
       A scan that finds the operator's own domain replaces it. */
    if (g_learned[0] == '\0' && g_imsi[0] != '\0')
        snprintf(g_learned, sizeof(g_learned), "%s", g_imsi);

    /* sip-learned without a domain would dial an empty R-URI domain: keep tel until one is seen.
       With the IMSI domain it dials the same as sip-imsi, only through the patched setter. */
    if (strcmp(g_mode, "sip-learned") == 0 && g_learned[0] == '\0')
        snprintf(g_mode, sizeof(g_mode), "tel");
    else if (strcmp(g_mode, "sip-learned") == 0 && strcmp(g_learned, g_imsi) == 0)
        snprintf(g_mode, sizeof(g_mode), "sip-imsi");

    g_ims_patch_home_domain = strcmp(g_mode, "sip-learned") == 0;

    /* Test for Telekom DE's 503 (1:223): data/rtcp-bw-<id> turns on the ims_patch.c site that
       gives our SDP non-zero b=RS/b=RR, as Telekom's own offers have. Set here, not with the DCA
       configs, because every start path runs this before ims_rrm starts. */
    g_ims_patch_rtcp_bw = knob_on(knob_find("rtcp-bw"));
    if (g_ims_patch_rtcp_bw)
        log_msg("[BBE] RTCP_BW: patch on for carrier %d", g_carrier_id);

    /* A request left from before this start was already handled (or is stale, like a debug
       run): only a newer one, written while the daemon runs, counts. This also stops a restart
       or fetch request from the app from running again after the daemon starts over for it. */
    {
        char text[1024], m[16], req[32] = {0}, k[32], a[16];
        int r, d;
        if (read_file(REQUEST_PATH, text, sizeof(text)) >= 0 &&
            volte_settings_parse_request(text, m, sizeof(m), req, sizeof(req), &r, &d, k,
                                         sizeof(k), a, sizeof(a)))
            snprintf(g_last_request, sizeof(g_last_request), "%s", req);
    }

    /* Calls from before this start are never evaluated: remember the last one in the log. */
    scan_log();
    g_last_call_id = g_call.id;

    mkdir_p(DOCS_DIR, 0777);
    g_inited = 1;
    write_status("idle", "");
}

/* Stores the mode and pushes the DCA config. The caller decides whether IMS restarts. */
static int store_mode(const char *mode)
{
    snprintf(g_mode, sizeof(g_mode), "%s", mode);
    g_ims_patch_home_domain = strcmp(g_mode, "sip-learned") == 0;
    if (g_carrier_id > 0)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/dial-modes", g_data_path);
        write_kv(path, g_carrier_id, g_mode);
    }

    write_dca_configs();
    return push_dca_configs();
}

/* Stores the mode and restarts IMS through the daemon's own path. */
static void apply_mode(const char *mode, const char *done_message)
{
    if (store_mode(mode) != 0)
    {
        write_status("error", "Could not load the new config (dca failed)");
        return;
    }
    write_status("applying", "Restarting IMS...");

    /* The daemon's own path: it stops ims_rrm, clears stale routes, restarts, and runs
       reset_radio_if_stalled() when a restart does not register. A raw stop/start left
       the phone needing airplane mode, because restarting ims_rrm alone does not clear
       the same-IP/port collision. A user Apply gets a fresh reset budget. */
    ims_reconnect_reset_radio_budget();
    int ok = reconnect_ims_with_backoff();
    if (!ok && volte_settings_voice_off())
        return; /* turned off meanwhile; that request wrote the status */
    write_status(ok ? "idle" : "error", ok ? done_message : "IMS did not connect");
}

/* After a call ends: in automatic mode, a refused address moves on to the next dial format
   and a connected call confirms the current one. */
static void auto_check_last_call(void)
{
    const char *used, *next;
    char letter, old[16];
    int learned_differs;

    scan_log();
    if (g_call.id == 0 || g_call.id == g_last_call_id || g_call.in_progress)
        return;
    g_last_call_id = g_call.id;

    if (strcmp(g_selection, "auto") != 0)
    {
        write_status("idle", NULL);
        return;
    }

    /* A call dialled with another mode than the current one predates a switch. */
    used = volte_call_mode(&g_call, g_imsi);
    if (!used || strcmp(used, g_mode) != 0)
        return;

    if (g_call.connected)
    {
        if (strcmp(g_auto_state, "confirmed") != 0)
        {
            snprintf(g_auto_state, sizeof(g_auto_state), "confirmed");
            save_auto();
            snprintf(g_auto_note, sizeof(g_auto_note), "Calls connect with %s", g_mode);
        }
        write_status("idle", NULL);
        return;
    }
    if (strcmp(g_auto_state, "testing") != 0 || !volte_call_refused(&g_call))
    {
        write_status("idle", NULL);
        return;
    }

    letter = volte_mode_letter(g_mode);
    if (letter && !strchr(g_failed, letter) && strlen(g_failed) < sizeof(g_failed) - 1)
    {
        size_t n = strlen(g_failed);
        g_failed[n] = letter;
        g_failed[n + 1] = '\0';
    }
    learned_differs = g_learned[0] && strcmp(g_learned, g_imsi) != 0;
    next = volte_auto_next(g_failed, learned_differs);

    if (!next)
    {
        /* The network takes no VoLTE call from this phone. Leave ims_rrm stopped so calls use
           3G or 2G, and keep the mode testing started with (the default, or a seeded one) for when
           VoLTE is turned back on. Nothing reconnects while off (voice_off() gates it). */
        const char *first = volte_mode_from_letter(g_failed[0]);

        snprintf(g_auto_state, sizeof(g_auto_state), "off");
        g_off_sim = g_sim_id;
        snprintf(g_auto_note, sizeof(g_auto_note),
                 "Network refused every dial format; VoLTE is off on this network and calls use 3G or 2G");
        save_auto();
        log_msg("[BBE] VOLTE_OFF: %s", g_auto_note);
        if (first && strcmp(first, g_mode) != 0 && store_mode(first) != 0)
            log_msg("[BBE] DCA_FAILED: VoLTE off");
        stop_ims();
        write_status("idle", NULL);
        return;
    }

    snprintf(old, sizeof(old), "%s", g_mode);
    snprintf(g_auto_note, sizeof(g_auto_note),
             "Network refused a call with %d using %s; switched to %s", g_call.code, old, next);
    save_auto();
    log_msg("[BBE] DIAL_AUTO: %s", g_auto_note);
    apply_mode(next, "Switched automatically");
}

/* "<name>:<0|1>" from the app: flip the flag file, then make it take effect. */
static void apply_knob(char *spec)
{
    char path[512];
    char *colon = strchr(spec, ':');
    const knob_t *k;
    int on;

    if (!colon)
    {
        write_status("error", "Unknown setting");
        return;
    }
    *colon = '\0';
    k = knob_find(spec);
    if (!k || (strcmp(colon + 1, "1") != 0 && strcmp(colon + 1, "0") != 0))
    {
        write_status("error", "Unknown setting");
        return;
    }
    on = colon[1] == '1';

    if (knob_path(k, path, sizeof(path)) != 0)
    {
        write_status("error", "Carrier not known yet");
        return;
    }
    if (knob_on(k) == on)
    {
        write_status("idle", "No change");
        return;
    }
    /* Checked before anything changes: the flag also decides how ims_rrm is patched. */
    if (strcmp(k->name, "rtcp-bw") == 0 && call_active())
    {
        write_status("error", "A call is in progress; try again");
        return;
    }

    if ((on ? write_file(path, "on\n", 0644) : unlink(path)) != 0)
    {
        write_status("error", "Could not save the setting");
        return;
    }
    log_msg("[BBE] KNOB: %s %s", k->name, on ? "on" : "off");

    if (strcmp(k->name, "force-ims-voice") == 0)
    {
        /* sync_voice_flag() runs every monitor loop and reads this. */
        g_force_ims_voice = on;
        write_status("idle", on ? "Saved" : "Saved; the phone drops the flag at its next network update");
        return;
    }

    /* rtcp-bw: ims_rrm_binary() patches ims_rrm on each start from this flag. */
    g_ims_patch_rtcp_bw = on;
    if (volte_settings_voice_off())
    {
        write_status("idle", "Saved; applies when VoLTE is on");
        return;
    }
    write_status("applying", "Restarting IMS...");
    ims_reconnect_reset_radio_budget();
    int ok = reconnect_ims_with_backoff();
    if (!ok && volte_settings_voice_off())
        return;
    write_status(ok ? "idle" : "error", ok ? "Saved; VoLTE restarted" : "IMS did not connect");
}

/* Downloads the carrier list now. A new list needs its XML built and reloaded, which only a
   clean start does, so the daemon starts over (as for a new SIM). A restart drops IMS, so not
   during a call. */
static void fetch_carrier_list(void)
{
    if (call_active())
    {
        write_status("error", "A call is in progress; try again");
        return;
    }
    write_status("applying", "Downloading the carrier list...");
    log_msg("[BBE] FETCH: carrier list requested from the settings app");
    if (refresh_carrier_db(1) < 0)
    {
        write_status("error", "Download failed. Connect to Wi-Fi or switch Mobile Data on, then try again");
        return;
    }
    write_status("applying", "Carrier list updated. Restarting VoLTE...");
    request_daemon_restart();
}

/* Same path as Apply with the stored format. With no carrier list yet there is no profile to
   restart with (first run, or a reinstall wiped it), so that fetches it first. */
static void restart_ims(void)
{
    if (call_active())
    {
        write_status("error", "A call is in progress; try again");
        return;
    }
    if (volte_settings_voice_off())
    {
        write_status("error", "VoLTE is off. Choose Automatic or a format and tap Apply to turn it on");
        return;
    }
    log_msg("[BBE] RESTART_IMS: requested from the settings app");
    if (!carrier_db_present())
    {
        fetch_carrier_list();
        return;
    }
    apply_mode(g_mode, "IMS restarted");
}

/* The dial format (g_mode) stays, so turning VoLTE back on returns to it. */
static void turn_off(void)
{
    if (call_active())
    {
        write_status("error", "A call is in progress; try again");
        return;
    }
    snprintf(g_selection, sizeof(g_selection), "off");
    save_auto();
    log_msg("[BBE] VOLTE_OFF: turned off in the settings app; calls use 3G or 2G");
    stop_ims();
    write_status("idle", "VoLTE is off; calls use 3G or 2G");
}

/* For the loops that keep the monitor from polling: the boot wait for mobile data and IMS
   reconnects, which can block for many minutes. Unanswered, the app stays busy and Apply is
   greyed out (Thomas on 2.42 could not turn VoLTE off). VoLTE off and the debug report run at
   once; the reconnect loop stops when VoLTE is off. Anything else is answered "busy" so the
   app can retry it later. */
void volte_settings_poll_busy(void)
{
    char text[1024];
    char mode[16] = {0};
    char request[32] = {0};
    char knob[32] = {0};
    char action[16] = {0};
    int refresh = 0, debug = 0;

    if (!g_inited || read_file(REQUEST_PATH, text, sizeof(text)) < 0)
        return;
    if (!volte_settings_parse_request(text, mode, sizeof(mode), request, sizeof(request),
                                      &refresh, &debug, knob, sizeof(knob), action, sizeof(action)))
        return;
    if (request[0] && strcmp(request, g_last_request) == 0)
        return;
    snprintf(g_last_request, sizeof(g_last_request), "%s", request);

    if (strcmp(mode, "off") == 0)
        turn_off();
    else if (debug)
        run_debug();
    else if (strcmp(action, "fetch") == 0)
        fetch_carrier_list();
    else if (strcmp(action, "restart") == 0 && g_lte_monitor_running && !volte_settings_voice_off())
    {
        /* Stuck reconnecting: drop the attempt and start clean. Before the monitor runs, the
           daemon is still starting up, which is the same thing. */
        if (call_active())
            write_status("error", "A call is in progress; try again");
        else
        {
            log_msg("[BBE] RESTART_IMS: requested from the settings app while reconnecting");
            write_status("applying", "Restarting VoLTE...");
            request_daemon_restart();
        }
    }
    else if (mode[0] || knob[0] || action[0])
        write_status("error", "VoLTE is still connecting; try again in a few minutes. VoLTE off works now.");
    else
        write_status("idle", "VoLTE is still connecting. VoLTE off works now.");
}

void volte_settings_poll(void)
{
    char text[1024];
    char mode[16] = {0};
    char request[32] = {0};
    char knob[32] = {0};
    char action[16] = {0};
    int refresh = 0, debug = 0;

    if (!g_inited)
        return;

    /* ims_rrm logs the dialog end slightly after the phone state changes. */
    if (g_phone_changed && !call_active())
    {
        g_phone_changed = 0;
        sleep(1);
        auto_check_last_call();
    }

    if (read_file(REQUEST_PATH, text, sizeof(text)) < 0)
        return;
    if (!volte_settings_parse_request(text, mode, sizeof(mode), request, sizeof(request),
                                      &refresh, &debug, knob, sizeof(knob), action, sizeof(action)))
        return;
    if (request[0] && strcmp(request, g_last_request) == 0)
        return;
    snprintf(g_last_request, sizeof(g_last_request), "%s", request);

    (void)refresh;
    scan_log();

    if (strcmp(action, "fetch") == 0)
    {
        fetch_carrier_list();
        return;
    }
    if (strcmp(action, "restart") == 0)
    {
        restart_ims();
        return;
    }

    /* Apply always reapplies, even the current mode: it is the user's way to bring the live
       config back in line with the stored mode. */
    if (mode[0])
    {
        if (strcmp(mode, "off") == 0)
        {
            turn_off();
            return;
        }
        if (strcmp(mode, "auto") == 0)
        {
            int was_off = volte_settings_voice_off();

            snprintf(g_selection, sizeof(g_selection), "auto");
            g_failed[0] = '\0';
            snprintf(g_auto_state, sizeof(g_auto_state), "testing");
            save_auto();
            if (!was_off)
            {
                write_status("idle", "Automatic selection on");
                return;
            }

            /* IMS was left stopped: bring it back, with a fresh reset budget like Apply. */
            g_auto_note[0] = '\0';
            write_status("applying", "Turning VoLTE back on...");
            ims_reconnect_reset_radio_budget();
            int ok = reconnect_ims_with_backoff();
            if (!ok && volte_settings_voice_off())
                return;
            write_status(ok ? "idle" : "error", ok ? "VoLTE is back on" : "IMS did not connect");
            return;
        }
        if (strcmp(mode, "tel") != 0 && strcmp(mode, "sip-imsi") != 0 &&
            strcmp(mode, "sip-learned") != 0)
        {
            write_status("error", "Unknown dial mode");
            return;
        }
        if (strcmp(mode, "sip-learned") == 0 && g_learned[0] == '\0')
        {
            write_status("error", "No learned domain seen on this network");
            return;
        }
        if (strcmp(mode, "sip-learned") == 0 && strcmp(g_learned, g_imsi) == 0)
        {
            write_status("error", "The learned domain is the IMSI domain: use sip-imsi");
            return;
        }
        if (call_active())
        {
            write_status("error", "A call is in progress; try again");
            return;
        }

        snprintf(g_selection, sizeof(g_selection), "manual");
        snprintf(g_auto_state, sizeof(g_auto_state), "testing");
        save_auto();
        apply_mode(mode, "Applied");
        return;
    }

    if (knob[0])
    {
        apply_knob(knob);
        return;
    }

    if (debug)
    {
        run_debug();
        return;
    }

    write_status("idle", "");
}

void volte_settings_phone_changed(void)
{
    g_phone_changed = 1;
}

const char *volte_settings_mode(void)
{
    return g_mode;
}

const char *volte_settings_domain(void)
{
    return strcmp(g_mode, "sip-learned") == 0 ? g_learned : "";
}

int volte_settings_voice_off(void)
{
    return strcmp(g_selection, "off") == 0 ||
           (strcmp(g_selection, "auto") == 0 && strcmp(g_auto_state, "off") == 0);
}

const char *volte_settings_selection(void)
{
    return g_selection;
}
