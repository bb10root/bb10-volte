#ifndef IMS_CARRIER_CONFIG_H
#define IMS_CARRIER_CONFIG_H

#include <stddef.h>

#define CARRIER_IDS_PATH "/base/etc/carrier/ids/carrierIds.xml"
#define IMS_CONFIG_DIR   "/var/ims/carrier"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parses carrierIds.xml and returns the carrier_id for the given MCC and MNC.
 * Uses libxml2 SAX API for high performance and low memory footprint.
 *
 * @param mcc Mobile Country Code (e.g., "255")
 * @param mnc Mobile Network Code (e.g., "01")
 * @return carrier_id (>0) on success, or -1 if not found or on error
 */
int ims_get_carrier_id(const char *mcc, const char *mnc);

/**
 * Resolves the path to the imsconfig_<carrier_id>.xml file for the given MCC/MNC.
 *
 * @param mcc Mobile Country Code
 * @param mnc Mobile Network Code
 * @param out_path Output buffer to store the generated path
 * @param max_len Maximum size of the out_path buffer
 * @return 0 on success, -1 on error or if the file does not exist
 */
int ims_resolve_config_path(const char *mcc, const char *mnc, char *out_path, size_t max_len);

/**
 * Sets the value of the <AllowVoLteProvisioning> node to "enabled" in the specified XML file.
 * Uses libxml2 XPath/DOM API.
 *
 * @param xml_path Full path to the imsconfig_*.xml file
 * @return 0 on success, -1 on error or if the node is not found
 */
int ims_enable_volte_provisioning(const char *xml_path);

#ifdef __cplusplus
}
#endif

#endif /* IMS_CARRIER_CONFIG_H */