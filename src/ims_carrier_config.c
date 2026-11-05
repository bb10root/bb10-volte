#include "ims_carrier_config.h"
#include "carrier_lookup.h"

#include <stdio.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

int ims_resolve_config_path(const char *mcc, const char *mnc, const char *gid1, char *out_path, size_t max_len)
{
    if (!out_path || max_len == 0)
        return -1;

    int carrier_id = ims_get_carrier_id(mcc, mnc, gid1);
    if (carrier_id > 0)
    {
        snprintf(out_path, max_len, "%s/imsconfig_%d.xml", IMS_CONFIG_DIR, carrier_id);
        if (access(out_path, F_OK) == 0)
        {
            return 0;
        }
    }

    /* Fallback if carrier_id is not found or corresponding XML file is missing */
    snprintf(out_path, max_len, "%s/imsconfig_0%s.xml", IMS_CONFIG_DIR, mnc);
    return (access(out_path, F_OK) == 0) ? 0 : -1;
}

int ims_enable_volte_provisioning(const char *xml_path)
{
    if (!xml_path || xml_path[0] == '\0')
        return -1;

    xmlInitParser();

    xmlDocPtr doc = xmlReadFile(xml_path, NULL, XML_PARSE_NOBLANKS);
    if (!doc)
    {
        return -1;
    }

    xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
    if (!xpathCtx)
    {
        xmlFreeDoc(doc);
        return -1;
    }

    xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression((const xmlChar *)"//AllowVoLteProvisioning", xpathCtx);
    if (!xpathObj)
    {
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return -1;
    }

    xmlNodeSetPtr nodes = xpathObj->nodesetval;
    int modified = 0;

    if (nodes && nodes->nodeNr > 0)
    {
        for (int i = 0; i < nodes->nodeNr; i++)
        {
            xmlNodeSetContent(nodes->nodeTab[i], (const xmlChar *)"enabled");
            modified = 1;
        }
    }

    int status = -1;
    if (modified)
    {
        if (xmlSaveFormatFileEnc(xml_path, doc, "UTF-8", 1) >= 0)
        {
            status = 0;
        }
    }

    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);

    return status;
}
