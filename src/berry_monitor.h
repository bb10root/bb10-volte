#ifndef BERRY_MONITOR_H
#define BERRY_MONITOR_H

#include <signal.h>

#define PPS_IMS "/pps/services/ims/status_public"
#define PPS_ICON "/pps/system/navigator/status/bb_service"
#define PPS_BBIO "/pps/services/bbio/status"

extern volatile sig_atomic_t g_berry_running;

int parse_ims_pps_buffer(char *buf);
int ims_connected_pps(void);
int icon_state(char *buf);

void start_berry(void);
void stop_berry(void);
int self_test(void);

#endif