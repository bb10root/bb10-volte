#ifndef IMS_RECONNECT_H
#define IMS_RECONNECT_H

int wait_connected(void);
int run_berry_check(void);
int reconnect_ims_with_backoff(void);
void check_ims_health(int *was_ready);

#endif /* IMS_RECONNECT_H */
