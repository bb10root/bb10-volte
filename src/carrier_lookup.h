#ifndef CARRIER_LOOKUP_H
#define CARRIER_LOOKUP_H

#define CARRIER_IDS_PATH   "/base/etc/carrier/ids/carrierIds.xml"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Finds the carrier_id for a SIM in a carrierIds.xml, the way carrier_mgr does: the longest
 * efgid that prefixes the SIM's GID1 wins, otherwise the network's row without an efgid.
 * Rows limited by IMSI range or ICCID are skipped, since the daemon can't check them.
 *
 * @param gid1 The SIM's GID1 in hex, or NULL/empty if unknown
 * @return carrier_id (>0) on success, or -1 if not found or on error
 */
int ims_find_carrier_id(const char *ids_path, const char *mcc, const char *mnc, const char *gid1);

/* ims_find_carrier_id() on the phone's own list, which the addon's link may replace. */
int ims_get_carrier_id(const char *mcc, const char *mnc, const char *gid1);

#ifdef __cplusplus
}
#endif

#endif /* CARRIER_LOOKUP_H */
