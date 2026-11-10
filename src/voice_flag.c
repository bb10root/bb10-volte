#include "voice_flag.h"
#include "sys_utils.h"
#include "berry_monitor.h"
#include "system_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* network_service bits, as tested by ims_rrm and named by cellular-control */
#define NET_SERVICE_DATA_LTE 0x200000UL
#define NET_SERVICE_IMS_VOICE 0x2000000UL

int g_force_ims_voice = 0;

/* --- Forced IMS voice (opt-in) --- */

/* ims_rrm refuses IMS calls without this bit. Some networks serve them anyway. */
unsigned long next_network_service(unsigned long service, int ims_up)
{
    if (ims_up && (service & NET_SERVICE_DATA_LTE))
        return service | NET_SERVICE_IMS_VOICE;

    return service & ~NET_SERVICE_IMS_VOICE;
}

/* cellular-control overwrites the bit whenever it republishes, so this keeps
 * reapplying it. Clearing it when IMS is down lets calls fall back to CS.
 * ponytail: read then write can briefly republish an old mask; the next change
 * from cellular-control corrects it. */
void sync_voice_flag(int allowed)
{
    static int logged = -1;
    char buf[2048];

    if (!g_force_ims_voice || read_file(PPS_RADIO_PATH, buf, sizeof(buf)) < 0)
        return;

    const char *key = "network_service:n:";
    const char *pos = strstr(buf, key);
    if (!pos)
        return;

    unsigned long service = strtoul(pos + strlen(key), NULL, 10);
    unsigned long next = next_network_service(service, allowed && g_current_state.lte_ok && ims_connected_pps());
    if (next != service)
    {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "[n]network_service:n:%lu\n", next);
        append_file(PPS_RADIO_PATH, cmd);
    }

    int on = (next & NET_SERVICE_IMS_VOICE) != 0;
    if (on != logged)
    {
        log_msg("[BBE] IMS_VOICE_FLAG: %s", on ? "ON" : "OFF");
        logged = on;
    }
}
