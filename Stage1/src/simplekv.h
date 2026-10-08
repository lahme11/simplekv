#ifndef SIMPLEKV_H
#define SIMPLEKV_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>

#define BUCKETS       1024
#define LINE_BUFSIZE  4096
#define MAX_ARGS      8
#define DEFAULT_LOG   "data.kvlog"

struct item {
    char        *key;
    size_t       key_len;
    char        *value;
    size_t       value_len;
    struct item *next;
};

struct item *table_get(const char *key, size_t key_len);
void         table_set(const char *key, size_t key_len, const char *value, size_t value_len);
int          table_delete(const char *key, size_t key_len);
size_t       table_count(void);
size_t       table_keys(struct item ***items_out);
void         table_free(void);
int          log_open(const char *path);
void         log_close(void);
int          log_set(const char *key, size_t key_len, const char *value, size_t value_len);
int          log_delete(const char *key, size_t key_len);

#endif
