#ifndef CARRIER_FETCH_H
#define CARRIER_FETCH_H

/* Downloads the AOSP lists over verified HTTPS, then generates. 0 on success. */
int ims_carrier_db_fetch(const char *stock_path, const char *out_db, const char *out_apns);

#endif /* CARRIER_FETCH_H */
