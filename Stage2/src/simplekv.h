#ifndef SIMPLEKV_H
#define SIMPLEKV_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>

#define MAX_KEY           1024
#define MAX_VALUE         1048576
#define LINE_MAX_LEN      1100000
#define MAX_ARGS          8
#define FILE_HEADER       8
#define RECORD_HEADER     21
#define FILE_MAGIC        "SKV2"
#define FILE_VERSION      1
#define AUTO_COMPACT_MIN  1048576
#define BATCH_SYNC_EVERY  100
#define INITIAL_BUCKETS   64
#define DEFAULT_DB        "data.skv"

#define RECORD_PUT     1
#define RECORD_DELETE  2

#define RECORD_OK      0
#define RECORD_SHORT   1
#define RECORD_BAD     2

struct record {
    uint8_t     type;
    uint32_t    key_len;
    uint32_t    value_len;
    int64_t     expires_at;
    const char *key;
    const char *value;
};

struct entry {
    char         *key;
    uint32_t      key_len;
    off_t         value_offset;
    uint32_t      value_len;
    int64_t       expires_at;
    uint32_t      record_len;
    struct entry *next;
};

struct index;

#define STORE_IO_ERROR     -1
#define STORE_TOO_BIG      -3
#define STORE_NOT_INTEGER  -4
#define STORE_OVERFLOW     -5

enum sync_mode { SYNC_ALWAYS, SYNC_BATCH, SYNC_NEVER };

struct store_stats {
    size_t keys;
    size_t records;
    off_t  file_size;
    off_t  live_bytes;
    off_t  dead_bytes;
    time_t last_compaction;
    enum sync_mode sync;
};

struct store;

uint32_t crc32(const void *data, size_t len);
void     put_u32(unsigned char *p, uint32_t v);
uint32_t get_u32(const unsigned char *p);
void     put_i64(unsigned char *p, int64_t v);
int64_t  get_i64(const unsigned char *p);
size_t   record_size(uint32_t key_len, uint32_t value_len);
size_t   record_encode(const struct record *r, unsigned char *out);
int      record_decode_header(const unsigned char *buf, size_t available, struct record *r, uint32_t *crc);
int      record_check(const unsigned char *whole, size_t len);

struct index *index_new(void);
void          index_free(struct index *ix);
struct entry *index_get(struct index *ix, const char *key, uint32_t len);
struct entry *index_put(struct index *ix, const char *key, uint32_t len);
int           index_remove(struct index *ix, const char *key, uint32_t len);
size_t        index_count(const struct index *ix);
size_t        index_buckets(const struct index *ix);
void          index_each(struct index *ix, void (*fn)(struct entry *, void *), void *ctx);

struct store *store_open(const char *path, enum sync_mode sync, time_t (*now)(void), char *err, size_t errlen);
void          store_close(struct store *db);
int           store_put(struct store *db, const char *key, uint32_t key_len, const char *value, uint32_t value_len,
                        int64_t expires_at);
int           store_get(struct store *db, const char *key, uint32_t key_len, char **value, uint32_t *value_len);
int           store_delete(struct store *db, const char *key, uint32_t key_len);
int           store_expire(struct store *db, const char *key, uint32_t key_len, int64_t seconds);
long long     store_ttl(struct store *db, const char *key, uint32_t key_len);
int           store_incr(struct store *db, const char *key, uint32_t key_len, long long by, long long *result);
int           store_keys(struct store *db, const char *prefix, uint32_t prefix_len, char ***keys, uint32_t **lens,
                         size_t *count);
void          store_free_keys(char **keys, uint32_t *lens, size_t count);
size_t        store_count(struct store *db);
int           store_compact(struct store *db, off_t *reclaimed);
off_t         store_take_auto_compaction(struct store *db);
void          store_stats(struct store *db, struct store_stats *stats);
const char   *store_recovery_note(struct store *db);
time_t        store_now(struct store *db);

#define COMMAND_OK     0
#define COMMAND_ERROR  1
#define COMMAND_QUIT   2
int run_command(struct store *db, int argc, char **argv, size_t *lens, FILE *out);

int  tokenize(char *line, char **argv, size_t *lens, int max, const char **error);
void print_quoted(FILE *out, const char *s, size_t len);

#endif
