#ifndef IMS_CONTROL_H
#define IMS_CONTROL_H

#define IR92_PATH "/var/dca/sources/ims/services/carrier_config/ims/ir92"

extern char g_ims_xml_path[256];

void provider_enable(void);
void stop_ims(void);
void start_ims(void);
void apply_volte_provisioning(void);
void write_dca_configs(void);

#endif /* IMS_CONTROL_H */
