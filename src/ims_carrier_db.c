#include "ims_carrier_db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#define TEXTPB_URL "https://android.googlesource.com/platform/packages/providers/TelephonyProvider/+/main/assets/latest_carrier_id/carrier_list.textpb?format=TEXT"
#define APNS_URL   "https://android.googlesource.com/device/sample/+/refs/heads/main/etc/apns-full-conf.xml?format=TEXT"
#define CA_PATH    "/etc/openssl/certs"

#define MAX_DOWNLOAD (8 * 1024 * 1024)
#define MIN_NEW_ROWS 100 /* Fewer means the upstream format changed: keep the old list. */
#define MAX_TUPLES   256
#define MAX_GIDS     32
#define GID_SIZE     40
#define NAME_SIZE    64

typedef struct
{
    int id;
    int mcc;
    int mnc;
    char gid[GID_SIZE];
    char name[NAME_SIZE];
} db_row_t;

typedef struct
{
    db_row_t *items;
    size_t count;
    size_t cap;
} row_list_t;

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
static int load_stock_rows(const char *stock_path, row_list_t *rows)
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
static int append_aosp_rows(FILE *f, const char *textpb, row_list_t *rows, row_list_t *aosp)
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

static int write_apns(const char *path, const row_list_t *rows, const row_list_t *aosp, const char *apns_xml,
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

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size)
    {
        buf[size] = '\0';
        *len = (size_t)size;
    }
    else
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

int ims_carrier_db_save_stock(const char *src, const char *dst)
{
    size_t len = 0;
    char tmp[600];
    char *data = read_all(src, &len);
    if (!data)
        return -1;

    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    FILE *f = fopen(tmp, "wb");
    int status = -1;
    if (f)
    {
        size_t written = fwrite(data, 1, len, f);
        if (fclose(f) == 0 && written == len)
            status = rename(tmp, dst);
    }
    free(data);
    return status;
}

int ims_carrier_db_generate(const char *textpb, const char *apns_xml, size_t apns_len,
                            const char *stock_path, const char *out_db, const char *out_apns)
{
    char tmp_db[600], tmp_apns[600];
    size_t stock_len = 0;
    row_list_t rows = {0};
    row_list_t aosp = {0};
    int status = -1;

    snprintf(tmp_db, sizeof(tmp_db), "%s.tmp", out_db);
    snprintf(tmp_apns, sizeof(tmp_apns), "%s.tmp", out_apns);

    xmlInitParser();

    char *stock = read_all(stock_path, &stock_len);
    if (!stock || load_stock_rows(stock_path, &rows) != 0)
        goto done;

    /* Last occurrence: everything before it stays exactly as BlackBerry shipped it. */
    char *tail = NULL;
    for (char *p = stock; (p = strstr(p, "</rows>")) != NULL; p++)
        tail = p;
    if (!tail)
        goto done;

    FILE *f = fopen(tmp_db, "wb");
    if (!f)
        goto done;

    fwrite(stock, 1, (size_t)(tail - stock), f);
    int added = append_aosp_rows(f, textpb, &rows, &aosp);
    fputs("</rows>\n", f);
    if (fclose(f) != 0 || added < MIN_NEW_ROWS)
        goto done;

    xmlDocPtr check = xmlReadFile(tmp_db, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
    if (!check)
        goto done;
    xmlFreeDoc(check);

    if (write_apns(tmp_apns, &rows, &aosp, apns_xml, apns_len) != 0)
        goto done;

    /* The list goes last: it is what makes the daemon look the new IDs up. */
    if (rename(tmp_apns, out_apns) == 0 && rename(tmp_db, out_db) == 0)
        status = 0;

done:
    if (status != 0)
    {
        remove(tmp_db);
        remove(tmp_apns);
    }
    free(rows.items);
    free(aosp.items);
    free(stock);
    return status;
}

typedef struct
{
    char *data;
    size_t len;
} download_t;

static size_t download_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    download_t *d = userdata;
    size_t bytes = size * nmemb;
    if (d->len + bytes > MAX_DOWNLOAD)
        return 0;

    char *data = realloc(d->data, d->len + bytes + 1);
    if (!data)
        return 0;

    memcpy(data + d->len, ptr, bytes);
    d->data = data;
    d->len += bytes;
    d->data[d->len] = '\0';
    return bytes;
}

/* Decodes in place, since gitiles serves ?format=TEXT as base64. Returns the decoded length. */
static size_t base64_decode(char *s)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned int bits = 0;
    int have = 0;
    size_t out = 0;

    for (const char *p = s; *p && *p != '='; p++)
    {
        const char *pos = strchr(table, *p);
        if (!pos)
            continue;

        bits = (bits << 6) | (unsigned int)(pos - table);
        have += 6;
        if (have >= 8)
        {
            have -= 8;
            s[out++] = (char)((bits >> have) & 0xff);
        }
    }
    s[out] = '\0';
    return out;
}

/*
 * The peer stays verified: this data decides which APN the phone uses and is read by root.
 * TLS 1.2 is what the phone's OpenSSL 1.0.2 negotiates by default; libcurl 7.24 has no option
 * to request it by name.
 * ponytail: relies on the OS GlobalSign root, which expires in 2028; ship a CA file in data/ if
 * the phone can no longer verify the chain by then.
 */
static int download(CURL *curl, const char *url, download_t *d)
{
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, d);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAPATH, CA_PATH);
    curl_easy_setopt(curl, CURLOPT_CAINFO, NULL);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bb10-volte");

    return curl_easy_perform(curl) == CURLE_OK && d->data ? 0 : -1;
}

int ims_carrier_db_fetch(const char *stock_path, const char *out_db, const char *out_apns)
{
    download_t textpb = {0}, apns = {0};
    int status = -1;

    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl = curl_easy_init();

    if (curl && download(curl, TEXTPB_URL, &textpb) == 0 && download(curl, APNS_URL, &apns) == 0)
    {
        base64_decode(textpb.data);
        size_t apns_len = base64_decode(apns.data);
        status = ims_carrier_db_generate(textpb.data, apns.data, apns_len, stock_path, out_db, out_apns);
    }

    if (curl)
        curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(textpb.data);
    free(apns.data);
    return status;
}
