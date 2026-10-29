#include "ims_carrier_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

/* -------------------------------------------------------------------------
 * SAX Parser Context & Callbacks (carrierIds.xml)
 * ------------------------------------------------------------------------- */

typedef struct
{
    const char *target_mcc;
    int target_mnc_num;

    int current_carrier_id;
    char current_mcc[8];
    char current_mnc[8];
    char current_text[128];

    int in_row;
    int matched_carrier_id;
} carrier_parse_ctx_t;

static void sax_start_element_cb(void *ctx, const xmlChar *name, const xmlChar **attrs)
{
    carrier_parse_ctx_t *pctx = (carrier_parse_ctx_t *)ctx;
    (void)attrs;

    if (pctx->matched_carrier_id > 0)
        return;

    if (xmlStrcmp(name, (const xmlChar *)"row") == 0)
    {
        pctx->in_row = 1;
        pctx->current_carrier_id = -1;
        pctx->current_mcc[0] = '\0';
        pctx->current_mnc[0] = '\0';
    }
    pctx->current_text[0] = '\0';
}

static void sax_characters_cb(void *ctx, const xmlChar *ch, int len)
{
    carrier_parse_ctx_t *pctx = (carrier_parse_ctx_t *)ctx;
    if (!pctx->in_row || pctx->matched_carrier_id > 0)
        return;

    size_t cur_len = strlen(pctx->current_text);
    if (cur_len + (size_t)len < sizeof(pctx->current_text))
    {
        strncat(pctx->current_text, (const char *)ch, (size_t)len);
    }
}

static void sax_end_element_cb(void *ctx, const xmlChar *name)
{
    carrier_parse_ctx_t *pctx = (carrier_parse_ctx_t *)ctx;
    if (!pctx->in_row || pctx->matched_carrier_id > 0)
        return;

    if (xmlStrcmp(name, (const xmlChar *)"carrier_id") == 0)
    {
        pctx->current_carrier_id = atoi(pctx->current_text);
    }
    else if (xmlStrcmp(name, (const xmlChar *)"mcc") == 0)
    {
        snprintf(pctx->current_mcc, sizeof(pctx->current_mcc), "%s", pctx->current_text);
    }
    else if (xmlStrcmp(name, (const xmlChar *)"mnc") == 0)
    {
        snprintf(pctx->current_mnc, sizeof(pctx->current_mnc), "%s", pctx->current_text);
    }
    else if (xmlStrcmp(name, (const xmlChar *)"row") == 0)
    {
        pctx->in_row = 0;

        if (pctx->current_carrier_id > 0 &&
            strcmp(pctx->current_mcc, pctx->target_mcc) == 0)
        {
            if (atoi(pctx->current_mnc) == pctx->target_mnc_num)
            {
                pctx->matched_carrier_id = pctx->current_carrier_id;
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API Implementation
 * ------------------------------------------------------------------------- */

int ims_get_carrier_id(const char *mcc, const char *mnc)
{
    if (!mcc || !mnc || mcc[0] == '\0' || mnc[0] == '\0')
        return -1;

    carrier_parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.target_mcc = mcc;
    ctx.target_mnc_num = atoi(mnc);
    ctx.current_carrier_id = -1;
    ctx.matched_carrier_id = -1;

    xmlSAXHandler sax_handler;
    memset(&sax_handler, 0, sizeof(sax_handler));
    sax_handler.startElement = sax_start_element_cb;
    sax_handler.endElement = sax_end_element_cb;
    sax_handler.characters = sax_characters_cb;

    xmlInitParser();

    if (xmlSAXUserParseFile(&sax_handler, &ctx, CARRIER_IDS_PATH) < 0)
    {
        return -1;
    }

    return ctx.matched_carrier_id;
}

int ims_resolve_config_path(const char *mcc, const char *mnc, char *out_path, size_t max_len)
{
    if (!out_path || max_len == 0)
        return -1;

    int carrier_id = ims_get_carrier_id(mcc, mnc);
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