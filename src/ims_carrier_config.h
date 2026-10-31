#ifndef IMS_CARRIER_CONFIG_H
#define IMS_CARRIER_CONFIG_H

#include <stddef.h>

#ifndef CARRIER_IDS_PATH
#define CARRIER_IDS_PATH "/base/etc/carrier/ids/carrierIds.xml"
#endif
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
 * Same as ims_get_carrier_id(), reading the given carrierIds.xml instead of CARRIER_IDS_PATH.
 */
int ims_get_carrier_id_from(const char *ids_path, const char *mcc, const char *mnc);

/**
 * Finds the carrier profile in data_dir. Prefers carrier-<carrier_id>-volte.xml; otherwise
 * falls back to the first carrier-*.xml found (single-profile packages).
 *
 * @return 0 and the full path in out_path, or -1 if data_dir has no carrier-*.xml
 */
int ims_find_carrier_profile(const char *data_dir, int carrier_id, char *out_path, size_t max_len);

/**
 * Fills out with the IR.92 DCA object to write for carrier_id: the contents of
 * data_dir/ir92-<carrier_id> when that file exists and starts with "@ir92", else default_cfg.
 *
 * @return 0 on success, -1 if the chosen config does not fit in max_len
 */
int ims_load_ir92_config(const char *data_dir, int carrier_id, const char *default_cfg,
                         char *out, size_t max_len);

/**
 * Checks that every attribute line of expected (after its "@ir92" header) appears as a whole
 * line in actual, in any order.
 *
 * @return 1 if applied, 0 otherwise
 */
int ims_ir92_applied(const char *actual, const char *expected);

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