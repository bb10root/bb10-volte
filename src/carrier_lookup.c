#include "carrier_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

/* -------------------------------------------------------------------------
 * SAX Parser Context & Callbacks (carrierIds.xml)
 * ------------------------------------------------------------------------- */

typedef struct
{
    const char *target_mcc;
    int target_mnc_num;
    const char *target_gid;

    int current_carrier_id;
    int current_limited;
    char current_mcc[8];
    char current_mnc[8];
    char current_gid[64];
    char current_text[128];

    int in_row;
    int gid_carrier_id;
    size_t gid_len;
    int network_carrier_id;
} carrier_parse_ctx_t;

static void sax_start_element_cb(void *ctx, const xmlChar *name, const xmlChar **attrs)
{
    carrier_parse_ctx_t *pctx = (carrier_parse_ctx_t *)ctx;
    (void)attrs;

    if (xmlStrcmp(name, (const xmlChar *)"row") == 0)
    {
        pctx->in_row = 1;
        pctx->current_carrier_id = -1;
        pctx->current_limited = 0;
        pctx->current_mcc[0] = '\0';
        pctx->current_mnc[0] = '\0';
        pctx->current_gid[0] = '\0';
    }
    else if (xmlStrcmp(name, (const xmlChar *)"imsirange") == 0 ||
             xmlStrcmp(name, (const xmlChar *)"iccidprefix") == 0)
    {
        pctx->current_limited = 1;
    }
    pctx->current_text[0] = '\0';
}

static void sax_characters_cb(void *ctx, const xmlChar *ch, int len)
{
    carrier_parse_ctx_t *pctx = (carrier_parse_ctx_t *)ctx;
    if (!pctx->in_row)
        return;

    size_t cur_len = strlen(pctx->current_text);
    if (cur_len + (size_t)len < sizeof(pctx->current_text))
    {
        strncat(pctx->current_text, (const char *)ch, (size_t)len);
    }
}

static void sax_finish_row(carrier_parse_ctx_t *pctx)
{
    if (pctx->current_carrier_id <= 0 || pctx->current_limited ||
        strcmp(pctx->current_mcc, pctx->target_mcc) != 0 ||
        atoi(pctx->current_mnc) != pctx->target_mnc_num)
    {
        return;
    }

    size_t gid_len = strlen(pctx->current_gid);
    if (gid_len == 0)
    {
        if (pctx->network_carrier_id <= 0)
            pctx->network_carrier_id = pctx->current_carrier_id;
    }
    else if (gid_len > pctx->gid_len && strncasecmp(pctx->target_gid, pctx->current_gid, gid_len) == 0)
    {
        pctx->gid_carrier_id = pctx->current_carrier_id;
        pctx->gid_len = gid_len;
    }
}

static void sax_end_element_cb(void *ctx, const xmlChar *name)
{
    carrier_parse_ctx_t *pctx = (carrier_parse_ctx_t *)ctx;
    if (!pctx->in_row)
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
    else if (xmlStrcmp(name, (const xmlChar *)"efgid") == 0)
    {
        snprintf(pctx->current_gid, sizeof(pctx->current_gid), "%s", pctx->current_text);
    }
    else if (xmlStrcmp(name, (const xmlChar *)"row") == 0)
    {
        pctx->in_row = 0;
        sax_finish_row(pctx);
    }
}

/* -------------------------------------------------------------------------
 * Public API Implementation
 * ------------------------------------------------------------------------- */

int ims_find_carrier_id(const char *ids_path, const char *mcc, const char *mnc, const char *gid1)
{
    if (!mcc || !mnc || mcc[0] == '\0' || mnc[0] == '\0')
        return -1;

    carrier_parse_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.target_mcc = mcc;
    ctx.target_mnc_num = atoi(mnc);
    ctx.target_gid = gid1 ? gid1 : "";
    ctx.current_carrier_id = -1;

    xmlSAXHandler sax_handler;
    memset(&sax_handler, 0, sizeof(sax_handler));
    sax_handler.startElement = sax_start_element_cb;
    sax_handler.endElement = sax_end_element_cb;
    sax_handler.characters = sax_characters_cb;

    xmlInitParser();

    if (xmlSAXUserParseFile(&sax_handler, &ctx, ids_path) < 0)
    {
        return -1;
    }

    if (ctx.gid_carrier_id > 0)
        return ctx.gid_carrier_id;
    return ctx.network_carrier_id > 0 ? ctx.network_carrier_id : -1;
}

int ims_get_carrier_id(const char *mcc, const char *mnc, const char *gid1)
{
    return ims_find_carrier_id(CARRIER_IDS_PATH, mcc, mnc, gid1);
}
