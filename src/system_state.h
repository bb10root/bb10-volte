#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#define PPS_RADIO_PATH "/pps/services/cellular/radioctrl/status_public"
#define PPS_UICC_PATH "/pps/services/cellular/uicc/card0/status_private"
#define PPS_DATA_PATH "/pps/services/cds/cnice/status_persistent"
#define PPS_CARRIER_MGR_PATH "/pps/services/carriermanager/status_private"
#define PPS_RADIO_CONTROL_PATH "/pps/services/cellular/radioctrl/control"

typedef struct
{
    int lte_ok;
    int sim_ok;
    int data_ok;
} system_state_t;

extern char g_mcc[4];
extern char g_mnc[4];
extern char g_gid1[41];
extern system_state_t g_current_state;

int pps_check_attribute_contains(const char *pps_path, const char *attr_name, const char *expected_val);
system_state_t get_current_system_state(void);
int is_state_ready(const system_state_t *st);
void log_state_transition(system_state_t old_st, system_state_t new_st);
int incoming_ready_pps(void);
void ready_check(void);
void wait_until_ready(void);

#endif /* SYSTEM_STATE_H */
