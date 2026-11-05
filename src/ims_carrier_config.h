#ifndef IMS_CARRIER_CONFIG_H
#define IMS_CARRIER_CONFIG_H

#include <stddef.h>

#define CARRIER_IDS_PATH   "/base/etc/carrier/ids/carrierIds.xml"
#define CARRIER_CONFIG_DIR "/base/etc/carrier/config"
#define IMS_CONFIG_DIR     "/var/ims/carrier"

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

/**
 * Resolves the path to the imsconfig_<carrier_id>.xml file for the given SIM.
 *
 * @param gid1 The SIM's GID1 in hex, or NULL/empty if unknown
 * @param out_path Output buffer to store the generated path
 * @param max_len Maximum size of the out_path buffer
 * @return 0 on success, -1 on error or if the file does not exist
 */
int ims_resolve_config_path(const char *mcc, const char *mnc, const char *gid1, char *out_path, size_t max_len);

/**
 * Sets the value of the <AllowVoLteProvisioning> node to "enabled" in the specified XML file.
 * Uses libxml2 XPath/DOM API.
 *
 * @param xml_path Full path to the imsconfig_*.xml file
 * @return 0 on success, -1 on error or if the node is not found
 */
int ims_enable_volte_provisioning(const char *xml_path);

/**
 * Builds a VoLTE profile for a carrier: its stock config, or the stock default config, with the
 * Android APNs from apns_path replacing what it has. With no Android APN a stock config keeps its
 * own, and the stock default stays empty for the user to fill in: the internet service is always
 * left editable. The IMS service is added unless the config already has
 * a real one. An unchanged file is not rewritten, because its timestamp is part of what the data
 * service compares.
 *
 * @param config_dir Directory holding the stock <id>.xml files, which may be links to profiles
 * @param apns_path  APN table written by ims_carrier_db_generate()
 * @return 0 on success, -1 on error
 */
int ims_build_carrier_xml(int carrier_id, const char *config_dir, const char *apns_path, const char *out_path);

#ifdef __cplusplus
}
#endif

#endif /* IMS_CARRIER_CONFIG_H */
