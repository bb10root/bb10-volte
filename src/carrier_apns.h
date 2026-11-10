#ifndef CARRIER_APNS_H
#define CARRIER_APNS_H

#include <stddef.h>

#include "carrier_rows.h"

int write_apns(const char *path, const row_list_t *rows, const row_list_t *aosp, const char *apns_xml,
               size_t apns_len);

#endif /* CARRIER_APNS_H */
