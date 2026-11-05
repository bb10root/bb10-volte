#ifndef CARRIER_PROFILE_H
#define CARRIER_PROFILE_H

#define CARRIER_CONFIG_DIR "/base/etc/carrier/config"

#ifdef __cplusplus
extern "C" {
#endif

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

#endif /* CARRIER_PROFILE_H */
