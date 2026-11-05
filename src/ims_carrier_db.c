#include "ims_carrier_db.h"
#include "carrier_apns.h"
#include "carrier_rows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#define MIN_NEW_ROWS 100 /* Fewer means the upstream format changed: keep the old list. */

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
