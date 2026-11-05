#include "system_state.h"
#include "sys_utils.h"
#include "berry_monitor.h"
#include "carrier_apply.h"
#include "ims_control.h"
#include "pps_monitor.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/pps.h>

char g_mcc[4] = {0};
char g_mnc[4] = {0};
char g_gid1[41] = {0};
system_state_t g_current_state = {0};

/* --- PPS checks and parsing --- */

int pps_check_attribute_contains(const char *pps_path, const char *attr_name, const char *expected_val)
{
    char buf[2048];
    if (read_file(pps_path, buf, sizeof(buf)) < 0)
        return 0;

    char *ptr = buf;
    pps_attrib_t info;
    const char *attrs[] = {attr_name, NULL};

    while (ppsparse(&ptr, NULL, attrs, &info, 0) > PPS_END)
    {
        if (info.attr_name && strcmp(info.attr_name, attr_name) == 0)
        {
            if (info.value && strstr(info.value, expected_val) != NULL)
            {
                return 1;
            }
        }
    }
    return 0;
}

system_state_t get_current_system_state(void)
{
    system_state_t state;
    state.lte_ok = pps_check_attribute_contains(PPS_RADIO_PATH, "network_technology", "lte");
    state.sim_ok = pps_check_attribute_contains(PPS_UICC_PATH, "uicc_state", "READY");
    state.data_ok = pps_check_attribute_contains(PPS_DATA_PATH, "data_service_mode", "ON");
    return state;
}

int is_state_ready(const system_state_t *st)
{
    return (st->lte_ok && st->sim_ok && st->data_ok);
}

void log_state_transition(system_state_t old_st, system_state_t new_st)
{
    int old_ready = is_state_ready(&old_st);
    int new_ready = is_state_ready(&new_st);

    if (old_ready && !new_ready)
    {
        if (!new_st.lte_ok)
        {
            log_msg("[BBE] DROPPED: LTE_LOST");
            LOG_VERBOSE("Connection dropped: LTE network lost");
        }
        if (!new_st.sim_ok)
        {
            log_msg("[BBE] DROPPED: SIM_NOT_READY");
            LOG_VERBOSE("Connection dropped: SIM status changed from READY");
        }
        if (!new_st.data_ok)
        {
            log_msg("[BBE] DROPPED: DATA_OFF");
            LOG_VERBOSE("Connection dropped: Cellular data disabled");
        }
    }
    else if (!old_ready && new_ready)
    {
        log_msg("[BBE] RESTORED: ALL_SERVICES_OK");
        LOG_VERBOSE("All conditions satisfied (LTE, SIM, Data). Restoring IMS...");
    }
}

int incoming_ready_pps(void)
{
    if (!pps_check_attribute_contains(PPS_IMS, "allowVoLteProvisioning", "1"))
        return 0;

    char buf2[256];
    return (read_file(IR92_PATH, buf2, sizeof(buf2)) >= 0 && strstr(buf2, "UsePreconditions:n:0") != NULL);
}

static int parse_json_key_value(const char *src, const char *key, char *out_val, size_t max_len)
{
    char search_pattern[32];
    snprintf(search_pattern, sizeof(search_pattern), "\"%s\":\"", key);

    const char *pos = strstr(src, search_pattern);
    if (!pos)
        return 0;

    pos += strlen(search_pattern);
    size_t i = 0;
    while (pos[i] >= '0' && pos[i] <= '9' && i < (max_len - 1))
    {
        out_val[i] = pos[i];
        i++;
    }
    out_val[i] = '\0';
    return i > 0;
}

static int detect_sim_carrier(void)
{
    char buf[2048];
    if (read_file(PPS_UICC_PATH, buf, sizeof(buf)) < 0)
        return 0;

    /* A SIM may have no GID1, and carriers then match on the network alone. */
    const char *gid1 = strstr(buf, "gid1::");
    g_gid1[0] = '\0';
    if (gid1)
    {
        size_t len = strspn(gid1 + 6, "0123456789abcdefABCDEF");
        if (len < sizeof(g_gid1))
        {
            memcpy(g_gid1, gid1 + 6, len);
            g_gid1[len] = '\0';
        }
    }

    const char *hplmn_pos = strstr(buf, "imsi_hplmn");
    if (!hplmn_pos)
        hplmn_pos = strstr(buf, "hplmn");

    if (hplmn_pos)
    {
        if (parse_json_key_value(hplmn_pos, "mcc", g_mcc, sizeof(g_mcc)) &&
            parse_json_key_value(hplmn_pos, "mnc", g_mnc, sizeof(g_mnc)))
        {
            return 1;
        }
    }
    return 0;
}

static void wait_for_lte(void)
{
    int printed_log = 0;
    while (!pps_check_attribute_contains(PPS_RADIO_PATH, "network_technology", "lte"))
    {
        if (!printed_log)
        {
            LOG_VERBOSE("LTE network not available. Waiting for LTE connection...");
            log_msg("[BBE] WAITING_FOR_LTE");
            printed_log = 1;
        }
        exit_if_stopped();
        sleep(3);
    }

    if (printed_log)
    {
        LOG_VERBOSE("LTE network detected. Proceeding...");
        log_msg("[BBE] LTE_CONNECTED");
    }
}

/* Returns the first unmet precondition other than LTE, or NULL. */
static const char *ready_failure(void)
{
    if (!pps_check_attribute_contains("/pps/services/phone/public/status", "phone_state", "idle"))
        return "CALL_ACTIVE";

    if (!pps_check_attribute_contains(PPS_UICC_PATH, "uicc_state", "READY"))
        return "SIM_NOT_READY";

    if (!detect_sim_carrier())
        return "SIM_UNSUPPORTED";

    if (!pps_check_attribute_contains(PPS_DATA_PATH, "data_service_mode", "ON"))
        return "DATA_OFF";

    return NULL;
}

void ready_check(void)
{
    const char *failure = ready_failure();
    if (failure)
        bbe_exit_fail(failure, 1);

    wait_for_lte();
}

/* Started at boot with nobody to retry: the SIM or data may just not be up yet. */
void wait_until_ready(void)
{
    const char *failure;
    const char *logged = NULL;

    while ((failure = ready_failure()) != NULL)
    {
        if (failure != logged)
        {
            log_msg("[BBE] NOT_READY: %s", failure);
            logged = failure;
        }
        exit_if_stopped();
        sleep(3);
    }

    fix_published_carrier_xml();
    wait_for_lte();
}
