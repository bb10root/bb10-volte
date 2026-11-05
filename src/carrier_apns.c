#include "carrier_apns.h"
#include "ims_carrier_db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

typedef struct
{
    int carrier_id; /* 0 when the entry only has mcc/mnc */
    int mcc;
    int mnc;
    int mvno;
    const char *type;
    const char *apn;
    const char *user;
    const char *password;
    const char *protocol;
    const char *networks;
} apn_t;

static const char *attr_value(xmlNodePtr node, const char *name)
{
    for (xmlAttrPtr a = node->properties; a; a = a->next)
    {
        if (strcmp((const char *)a->name, name) == 0 && a->children)
            return (const char *)a->children->content;
    }
    return NULL;
}

static apn_t *load_apns(xmlDocPtr doc, size_t *count)
{
    size_t total = 0, n = 0;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root)
        return NULL;

    for (xmlNodePtr node = root->children; node; node = node->next)
        total += node->type == XML_ELEMENT_NODE;

    apn_t *apns = calloc(total ? total : 1, sizeof(*apns));
    if (!apns)
        return NULL;

    for (xmlNodePtr node = root->children; node; node = node->next)
    {
        if (node->type != XML_ELEMENT_NODE || strcmp((const char *)node->name, "apn") != 0)
            continue;

        const char *visible = attr_value(node, "user_visible");
        const char *cid = attr_value(node, "carrier_id");
        const char *mcc = attr_value(node, "mcc");
        const char *mnc = attr_value(node, "mnc");
        if (visible && strcmp(visible, "false") == 0)
            continue;

        apn_t *a = &apns[n++];
        a->carrier_id = cid ? atoi(cid) : 0;
        a->mcc = mcc ? atoi(mcc) : -1;
        a->mnc = mnc ? atoi(mnc) : -1;
        a->mvno = attr_value(node, "mvno_type") != NULL;
        a->type = attr_value(node, "type");
        a->apn = attr_value(node, "apn");
        a->user = attr_value(node, "user");
        a->password = attr_value(node, "password");
        a->protocol = attr_value(node, "protocol");
        a->networks = attr_value(node, "network_type_bitmask");
    }

    *count = n;
    return apns;
}

/* LTE is network type 13. No list at all means every network. */
static int allows_lte(const char *networks)
{
    if (!networks)
        return 1;

    for (const char *p = networks; *p; p += strcspn(p, "|"), p += (*p == '|'))
    {
        if (atoi(p) == 13)
            return 1;
    }
    return 0;
}

static int has_type(const char *types, const char *wanted)
{
    size_t len = strlen(wanted);
    for (const char *p = types; p && *p; p += strcspn(p, ","), p += (*p == ','))
    {
        while (*p == ' ')
            p++;
        if (strncmp(p, wanted, len) == 0 && (p[len] == '\0' || p[len] == ',' || p[len] == ' '))
            return 1;
    }
    return 0;
}

/* Downloaded text ends up in a file the phone's data service reads. */
static int valid_apn_name(const char *s)
{
    /* Android lists a literal "default" for some carriers: a placeholder, not an APN. */
    if (!s || !*s || strlen(s) > 100 || strcasecmp(s, "default") == 0)
        return 0;
    for (; *s; s++)
    {
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' && *s != '_')
            return 0;
    }
    return 1;
}

static int valid_credential(const char *s)
{
    if (!s)
        return 1;
    if (strlen(s) > 100)
        return 0;
    for (; *s; s++)
    {
        if ((unsigned char)*s < 0x20 || *s == 0x7f)
            return 0;
    }
    return 1;
}

/* Most stock profiles are IPv4 only, so an IPv6-only APN would leave the phone with no data. */
static int apn_usable(const apn_t *a, const char *type)
{
    return has_type(a->type, type) && valid_apn_name(a->apn) &&
           valid_credential(a->user) && valid_credential(a->password) &&
           allows_lte(a->networks) && !(a->protocol && strcmp(a->protocol, "IPV6") == 0);
}

static int carrier_has_tagged_apn(const apn_t *apns, size_t count, int id, const char *type)
{
    for (size_t i = 0; i < count; i++)
    {
        if (apns[i].carrier_id == id - AOSP_ID_BASE && has_type(apns[i].type, type))
            return 1;
    }
    return 0;
}

/*
 * Prefers entries tagged with this carrier. Only a carrier with none falls back to the network's own
 * APN: one whose tagged APN is unusable must not borrow another on its network.
 */
static const apn_t *pick_apn(const apn_t *apns, size_t count, const row_list_t *rows, int id, const char *type)
{
    int aosp_id = id - AOSP_ID_BASE;

    for (size_t i = 0; i < count; i++)
    {
        if (apns[i].carrier_id == aosp_id && apn_usable(&apns[i], type))
            return &apns[i];
    }

    if (carrier_has_tagged_apn(apns, count, id, type))
        return NULL;

    for (size_t r = 0; r < rows->count; r++)
    {
        const db_row_t *row = &rows->items[r];
        if (row->id != id || row->gid[0] != '\0')
            continue;

        for (size_t i = 0; i < count; i++)
        {
            const apn_t *a = &apns[i];
            if (!a->carrier_id && !a->mvno && a->mcc == row->mcc && a->mnc == row->mnc && apn_usable(a, type))
                return a;
        }
    }
    return NULL;
}

/*
 * The Android APN for one stock row. A row limited to a GID only takes an exact Android match. A
 * carrier whose own APN is unusable stays on stock, rather than borrowing another APN on its network.
 */
static const apn_t *stock_row_apn(const apn_t *apns, size_t count, const row_list_t *aosp, const db_row_t *row,
                                  const char *type)
{
    for (size_t i = 0; i < aosp->count; i++)
    {
        const db_row_t *candidate = &aosp->items[i];
        if (candidate->mcc != row->mcc || candidate->mnc != row->mnc || strcasecmp(candidate->gid, row->gid) != 0)
            continue;

        const apn_t *found = pick_apn(apns, count, aosp, candidate->id, type);
        if (found)
            return found;
        if (carrier_has_tagged_apn(apns, count, candidate->id, type))
            return NULL;
    }

    if (row->gid[0] != '\0')
        return NULL;

    /* Anything on the network that isn't a virtual operator is the network's own. */
    for (size_t i = 0; i < count; i++)
    {
        const apn_t *a = &apns[i];
        if (!a->mvno && a->mcc == row->mcc && a->mnc == row->mnc && apn_usable(a, type))
            return a;
    }
    return NULL;
}

/*
 * One carrier ID has one profile, so it needs one answer: rows that disagree leave it on stock. So
 * does an answer from half of its rows or fewer, since a stock carrier's odd network code can belong to
 * another operator by now.
 */
static const apn_t *stock_carrier_apn(const apn_t *apns, size_t count, const row_list_t *rows, const row_list_t *aosp,
                                      int id, const char *type)
{
    const apn_t *chosen = NULL;
    int total = 0, agreeing = 0;

    for (size_t r = 0; r < rows->count; r++)
    {
        if (rows->items[r].id != id)
            continue;

        total++;
        const apn_t *found = stock_row_apn(apns, count, aosp, &rows->items[r], type);
        if (!found)
            continue;
        if (chosen && strcmp(found->apn, chosen->apn) != 0)
            return NULL;

        chosen = found;
        agreeing++;
    }
    return agreeing * 2 > total ? chosen : NULL;
}

static int id_seen_before(const row_list_t *rows, size_t index)
{
    for (size_t i = 0; i < index; i++)
    {
        if (rows->items[i].id == rows->items[index].id)
            return 1;
    }
    return 0;
}

static void write_apn_fields(FILE *f, const apn_t *a)
{
    fprintf(f, "\t%s\t%s\t%s", a ? a->apn : "", a && a->user ? a->user : "", a && a->password ? a->password : "");
}

int write_apns(const char *path, const row_list_t *rows, const row_list_t *aosp, const char *apns_xml,
                      size_t apns_len)
{
    xmlDocPtr doc = xmlReadMemory(apns_xml, (int)apns_len, "apns.xml", NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    if (!doc)
        return -1;

    size_t count = 0;
    apn_t *apns = load_apns(doc, &count);
    FILE *f = apns ? fopen(path, "w") : NULL;
    int status = -1;

    if (f)
    {
        for (size_t i = 0; i < rows->count; i++)
        {
            const db_row_t *row = &rows->items[i];
            if (id_seen_before(rows, i))
                continue;

            int is_new = row->id >= AOSP_ID_BASE;
            const apn_t *internet = is_new ? pick_apn(apns, count, aosp, row->id, "default")
                                           : stock_carrier_apn(apns, count, rows, aosp, row->id, "default");
            const apn_t *mms = is_new ? pick_apn(apns, count, aosp, row->id, "mms")
                                      : stock_carrier_apn(apns, count, rows, aosp, row->id, "mms");
            if (!internet && !mms)
                continue;

            /* A stock carrier keeps the name it shipped with, which is empty here. */
            char name[NAME_SIZE];
            snprintf(name, sizeof(name), "%s", row->name);
            name[strcspn(name, "\t\r\n")] = '\0';
            fprintf(f, "%d\t%s", row->id, name);
            write_apn_fields(f, internet);
            write_apn_fields(f, mms);
            fputc('\n', f);
        }
        status = fclose(f) == 0 ? 0 : -1;
    }

    free(apns);
    xmlFreeDoc(doc);
    return status;
}
