#ifndef IMS_CARRIER_CONFIG_H
#define IMS_CARRIER_CONFIG_H

#include <stddef.h>

#define IMS_CONFIG_DIR     "/var/ims/carrier"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* IMS_CARRIER_CONFIG_H */
