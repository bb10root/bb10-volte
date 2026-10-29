#ifndef IMS_CARRIER_CONFIG_H
#define IMS_CARRIER_CONFIG_H

#include <stddef.h>

#define CARRIER_IDS_PATH "/base/etc/carrier/ids/carrierIds.xml"
#define IMS_CONFIG_DIR   "/var/ims/carrier"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Парсить carrierIds.xml та повертає carrier_id за заданими MCC та MNC.
 * Використовує libxml2 SAX API для швидкодії та економії пам'яті.
 *
 * @param mcc Код країни (наприклад, "255")
 * @param mnc Код мережі (наприклад, "01")
 * @return carrier_id (>0) у разі успіху, або -1 якщо не знайдено / помилка
 */
int ims_get_carrier_id(const char *mcc, const char *mnc);

/**
 * Знаходить шлях до файла imsconfig_<carrier_id>.xml для вказаних MCC/MNC.
 *
 * @param mcc Код країни
 * @param mnc Код мережі
 * @param out_path Буфер для збереження сформованого шляху
 * @param max_len Максимальний розмір буфера out_path
 * @return 0 у разі успіху, -1 у разі помилки або відсутності файла
 */
int ims_resolve_config_path(const char *mcc, const char *mnc, char *out_path, size_t max_len);

/**
 * Змінює значення вузла <AllowVoLteProvisioning> на "enabled" у вказаному XML-файлі.
 * Використовує libxml2 XPath/DOM API.
 *
 * @param xml_path Повний шлях до файла imsconfig_*.xml
 * @return 0 у разі успіху, -1 у разі помилки або якщо вузол не знайдено
 */
int ims_enable_volte_provisioning(const char *xml_path);

#ifdef __cplusplus
}
#endif

#endif /* IMS_CARRIER_CONFIG_H */