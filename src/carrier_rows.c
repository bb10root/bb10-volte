#include "carrier_rows.h"
#include "ims_carrier_db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#define MAX_TUPLES   256
#define MAX_GIDS     32

static int row_add(row_list_t *list, const db_row_t *row)
{
    if (list->count == list->cap)
    {
        size_t cap = list->cap ? list->cap * 2 : 2048;
        db_row_t *items = realloc(list->items, cap * sizeof(*items));
        if (!items)
            return -1;
        list->items = items;
        list->cap = cap;
    }
    list->items[list->count++] = *row;
    return 0;
}

static int row_exists(const row_list_t *list, int mcc, int mnc, const char *gid)
{
    for (size_t i = 0; i < list->count; i++)
    {
        const db_row_t *row = &list->items[i];
        if (row->mcc == mcc && row->mnc == mnc && strcasecmp(row->gid, gid) == 0)
            return 1;
    }
    return 0;
}

/* Whether the stock list already has a row for the whole network. */
static int stock_covers_network(const row_list_t *list, int mcc, int mnc)
{
    for (size_t i = 0; i < list->count; i++)
    {
        const db_row_t *row = &list->items[i];
        if (row->id < AOSP_ID_BASE && row->mcc == mcc && row->mnc == mnc && row->gid[0] == '\0')
            return 1;
    }
    return 0;
}

static xmlNodePtr child_element(xmlNodePtr parent, const char *name)
{
    for (xmlNodePtr n = parent->children; n; n = n->next)
    {
        if (n->type == XML_ELEMENT_NODE && strcmp((const char *)n->name, name) == 0)
            return n;
    }
    return NULL;
}

static void child_text(xmlNodePtr parent, const char *name, char *out, size_t size)
{
    out[0] = '\0';
    xmlNodePtr node = child_element(parent, name);
    if (!node)
        return;

    xmlChar *text = xmlNodeGetContent(node);
    if (text)
    {
        snprintf(out, size, "%s", (const char *)text);
        xmlFree(text);
    }
}

static int all_digits(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
    {
        if (!isdigit((unsigned char)*s))
            return 0;
    }
    return 1;
}

/* Rows limited by IMSI range or ICCID don't stand for the whole network, so they don't count. */
int load_stock_rows(const char *stock_path, row_list_t *rows)
{
    xmlDocPtr doc = xmlReadFile(stock_path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    if (!doc)
        return -1;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr n = root ? root->children : NULL; n; n = n->next)
    {
        if (n->type != XML_ELEMENT_NODE || strcmp((const char *)n->name, "row") != 0)
            continue;
        if (child_element(n, "imsirange") || child_element(n, "iccidprefix"))
            continue;

        char mcc[8], mnc[8], id[16];
        db_row_t row;
        memset(&row, 0, sizeof(row));
        child_text(n, "carrier_id", id, sizeof(id));
        child_text(n, "mcc", mcc, sizeof(mcc));
        child_text(n, "mnc", mnc, sizeof(mnc));
        child_text(n, "efgid", row.gid, sizeof(row.gid));
        if (!all_digits(mcc) || !all_digits(mnc))
            continue; /* CDMA SID rows */

        row.id = atoi(id);
        row.mcc = atoi(mcc);
        row.mnc = atoi(mnc);
        if (row_add(rows, &row) != 0)
        {
            xmlFreeDoc(doc);
            return -1;
        }
    }

    xmlFreeDoc(doc);
    return 0;
}

static void write_escaped(FILE *f, const char *s)
{
    for (; *s; s++)
    {
        switch (*s)
        {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        default: fputc(*s, f); break;
        }
    }
}

static int parse_quoted(const char *line, char *out, size_t size)
{
    const char *start = strchr(line, '"');
    const char *end = strrchr(line, '"');
    if (!start || end <= start || (size_t)(end - start - 1) >= size)
        return 0;

    memcpy(out, start + 1, (size_t)(end - start - 1));
    out[end - start - 1] = '\0';
    return 1;
}

static int is_hex(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
    {
        if (!isxdigit((unsigned char)*s))
            return 0;
    }
    return 1;
}

static void add_tuple_rows(FILE *f, row_list_t *rows, row_list_t *aosp, const db_row_t *base, const char *tuple,
                           const char *gid, int *added)
{
    char mcc[4];
    if ((strlen(tuple) != 5 && strlen(tuple) != 6) || !all_digits(tuple))
        return;

    memcpy(mcc, tuple, 3);
    mcc[3] = '\0';

    db_row_t row = *base;
    row.mcc = atoi(mcc);
    row.mnc = atoi(tuple + 3);
    snprintf(row.gid, sizeof(row.gid), "%s", gid);

    /* Kept even when no row is added: it is how a stock carrier finds its Android APNs. */
    if (row_add(aosp, &row) != 0)
        return;

    /*
     * A GID row outranks the network's row, and a new ID has no stock config. On a network the
     * stock list covers, it would cost that SIM its working config, so those SIMs stay as they are.
     */
    if (gid[0] != '\0' && stock_covers_network(rows, row.mcc, row.mnc))
        return;

    if (row_exists(rows, row.mcc, row.mnc, row.gid) || row_add(rows, &row) != 0)
        return;

    fprintf(f, "  <row>\n    <carrier_id>%d</carrier_id>\n    <carrier_name>", row.id);
    write_escaped(f, row.name);
    fprintf(f, "</carrier_name>\n    <vendor_id>01</vendor_id>\n    <sap_sold_id></sap_sold_id>\n"
               "    <mcc>%03d</mcc>\n    <mnc>%03d</mnc>\n    <efgid>%s</efgid>\n  </row>\n",
            row.mcc, row.mnc, row.gid);
    (*added)++;
}

/*
 * Walks carrier_list.textpb line by line. Only attributes made of mccmnc_tuple and gid1 become
 * rows: one also limited by SPN or IMSI prefix describes a virtual operator, and a plain row
 * for it would capture every SIM on the host network.
 */
int append_aosp_rows(FILE *f, const char *textpb, row_list_t *rows, row_list_t *aosp)
{
    static char tuples[MAX_TUPLES][8];
    static char gids[MAX_GIDS][GID_SIZE];
    int tuple_count = 0, gid_count = 0, in_attr = 0, limited = 0, added = 0;
    db_row_t base;
    memset(&base, 0, sizeof(base));

    char *copy = strdup(textpb);
    if (!copy)
        return -1;

    for (char *next, *line = copy; line; line = next)
    {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '\0' || *line == '#')
            continue;

        if (strcmp(line, "carrier_id {") == 0)
        {
            memset(&base, 0, sizeof(base));
        }
        else if (strncmp(line, "carrier_attribute {", 19) == 0)
        {
            in_attr = 1;
            limited = 0;
            tuple_count = gid_count = 0;
        }
        else if (in_attr && line[0] == '}')
        {
            in_attr = 0;
            if (limited || base.id == 0)
                continue;

            for (int i = 0; i < tuple_count; i++)
            {
                if (gid_count == 0)
                    add_tuple_rows(f, rows, aosp, &base, tuples[i], "", &added);
                for (int g = 0; g < gid_count; g++)
                    add_tuple_rows(f, rows, aosp, &base, tuples[i], gids[g], &added);
            }
        }
        else if (in_attr)
        {
            if (strncmp(line, "mccmnc_tuple:", 13) == 0 && tuple_count < MAX_TUPLES &&
                parse_quoted(line, tuples[tuple_count], sizeof(tuples[0])))
            {
                tuple_count++;
            }
            else if (strncmp(line, "gid1:", 5) == 0 && gid_count < MAX_GIDS &&
                     parse_quoted(line, gids[gid_count], sizeof(gids[0])) && is_hex(gids[gid_count]))
            {
                gid_count++;
            }
            else
            {
                limited = 1;
            }
        }
        else if (strncmp(line, "canonical_id:", 13) == 0)
        {
            base.id = AOSP_ID_BASE + atoi(line + 13);
        }
        else if (strncmp(line, "carrier_name:", 13) == 0 &&
                 !parse_quoted(line, base.name, sizeof(base.name)))
        {
            snprintf(base.name, sizeof(base.name), "Carrier %d", base.id - AOSP_ID_BASE);
        }
    }

    free(copy);
    return added;
}
