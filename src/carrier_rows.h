#ifndef CARRIER_ROWS_H
#define CARRIER_ROWS_H

#include <stddef.h>
#include <stdio.h>

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

int load_stock_rows(const char *stock_path, row_list_t *rows);
int append_aosp_rows(FILE *f, const char *textpb, row_list_t *rows, row_list_t *aosp);

#endif /* CARRIER_ROWS_H */
