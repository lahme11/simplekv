#include "simplekv.h"

#include <libgen.h>

struct store {
    char          *path;
    int            fd;
    enum sync_mode sync;
    time_t       (*now)(void);
    struct index  *ix;
    off_t          end;
    off_t          live_bytes;
    size_t         records;
    size_t         unsynced;
    time_t         last_compaction;
    off_t          auto_compacted;
    char           note[200];
};

static void set_error(char *err, size_t errlen, const char *fmt, const char *detail)
{
    if (err != NULL && errlen > 0)
        snprintf(err, errlen, fmt, detail);
}

static int pwrite_all(int fd, const void *buf, size_t len, off_t offset)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= n;
        offset += n;
    }
    return 0;
}

static int pread_all(int fd, void *buf, size_t len, off_t offset)
{
    char *p = buf;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
        offset += n;
    }
    return 0;
}

static void make_header(unsigned char *header)
{
    memcpy(header, FILE_MAGIC, 4);
    header[4] = FILE_VERSION;
    header[5] = header[6] = header[7] = 0;
}

static void index_record(struct store *db, const struct record *r, off_t offset, size_t total)
{
    struct entry *old = index_get(db->ix, r->key, r->key_len);
    if (old != NULL)
        db->live_bytes -= old->record_len;
    struct entry *e = index_put(db->ix, r->key, r->key_len);
    e->value_offset = offset + RECORD_HEADER + r->key_len;
    e->value_len = r->value_len;
    e->expires_at = r->expires_at;
    e->record_len = (uint32_t)total;
    db->live_bytes += total;
}

static void unindex(struct store *db, const char *key, uint32_t key_len)
{
    struct entry *e = index_get(db->ix, key, key_len);
    if (e != NULL) {
        db->live_bytes -= e->record_len;
        index_remove(db->ix, key, key_len);
    }
}

static int load(struct store *db, off_t size)
{
    off_t offset = FILE_HEADER;
    unsigned char header[RECORD_HEADER];
    unsigned char *buf = NULL;
    size_t buf_size = 0;

    while (offset < size) {
        struct record r;
        uint32_t crc;
        off_t left = size - offset;
        int status = RECORD_SHORT;
        if (left >= RECORD_HEADER && pread_all(db->fd, header, RECORD_HEADER, offset) == 0)
            status = record_decode_header(header, RECORD_HEADER, &r, &crc);
        size_t total = status == RECORD_OK ? record_size(r.key_len, r.value_len) : 0;
        if (status == RECORD_OK && (off_t)total > left)
            status = RECORD_SHORT;
        if (status == RECORD_OK) {
            if (total > buf_size) {
                unsigned char *grown = realloc(buf, total);
                if (grown == NULL) {
                    free(buf);
                    return -1;
                }
                buf = grown;
                buf_size = total;
            }
            if (pread_all(db->fd, buf, total, offset) != 0 || !record_check(buf, total))
                status = RECORD_BAD;
        }
        if (status != RECORD_OK) {
            snprintf(db->note, sizeof(db->note), "recovered: removed %lld damaged bytes at offset %lld",
                     (long long)left, (long long)offset);
            if (ftruncate(db->fd, offset) != 0 || fsync(db->fd) != 0) {
                free(buf);
                return -1;
            }
            break;
        }
        r.key = (const char *)buf + RECORD_HEADER;
        if (r.type == RECORD_PUT)
            index_record(db, &r, offset, total);
        else
            unindex(db, r.key, r.key_len);
        db->records++;
        offset += total;
    }
    free(buf);
    db->end = offset;
    return 0;
}

struct store *store_open(const char *path, enum sync_mode sync, time_t (*now)(void), char *err, size_t errlen)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        set_error(err, errlen, "cannot open the database: %s", strerror(errno));
        return NULL;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        set_error(err, errlen, "%s", errno == EWOULDBLOCK ? "database is locked by another process"
                                                           : strerror(errno));
        close(fd);
        return NULL;
    }
    struct stat st;
    unsigned char header[FILE_HEADER], expected[FILE_HEADER];
    make_header(expected);
    if (fstat(fd, &st) != 0) {
        set_error(err, errlen, "cannot read the database: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    if (st.st_size == 0) {
        if (pwrite_all(fd, expected, FILE_HEADER, 0) != 0 || fsync(fd) != 0) {
            set_error(err, errlen, "cannot write the database: %s", strerror(errno));
            close(fd);
            return NULL;
        }
        st.st_size = FILE_HEADER;
    } else if (st.st_size < FILE_HEADER || pread_all(fd, header, FILE_HEADER, 0) != 0 ||
               memcmp(header, expected, 5) != 0) {
        set_error(err, errlen, "%s is not a SimpleKV file (or is from a different version)", path);
        close(fd);
        return NULL;
    }

    struct store *db = calloc(1, sizeof(*db));
    if (db == NULL || (db->path = strdup(path)) == NULL) {
        free(db);
        close(fd);
        set_error(err, errlen, "%s", "out of memory");
        return NULL;
    }
    db->fd = fd;
    db->sync = sync;
    db->now = now;
    db->ix = index_new();
    if (load(db, st.st_size) != 0) {
        set_error(err, errlen, "cannot read the database: %s", strerror(errno));
        store_close(db);
        return NULL;
    }
    return db;
}

time_t store_now(struct store *db)
{
    return db->now ? db->now() : time(NULL);
}

static int maybe_sync(struct store *db)
{
    if (db->sync == SYNC_ALWAYS)
        return fsync(db->fd);
    if (db->sync == SYNC_BATCH && ++db->unsynced >= BATCH_SYNC_EVERY) {
        db->unsynced = 0;
        return fsync(db->fd);
    }
    return 0;
}

static int append(struct store *db, const struct record *r, off_t *offset)
{
    size_t total = record_size(r->key_len, r->value_len);
    unsigned char *buf = malloc(total);
    if (buf == NULL)
        return -1;
    record_encode(r, buf);
    int rc = pwrite_all(db->fd, buf, total, db->end);
    free(buf);
    if (rc != 0) {
        if (ftruncate(db->fd, db->end) != 0)
            perror("simplekv: ftruncate");
        return -1;
    }
    *offset = db->end;
    db->end += total;
    db->records++;
    return maybe_sync(db);
}

static void maybe_auto_compact(struct store *db)
{
    off_t data = db->end - FILE_HEADER;
    if (db->end > AUTO_COMPACT_MIN && (data - db->live_bytes) * 2 > data) {
        off_t reclaimed = 0;
        if (store_compact(db, &reclaimed) == 0)
            db->auto_compacted += reclaimed;
    }
}

static struct entry *lookup(struct store *db, const char *key, uint32_t key_len)
{
    struct entry *e = index_get(db->ix, key, key_len);
    if (e != NULL && e->expires_at != 0 && e->expires_at <= store_now(db)) {
        unindex(db, key, key_len);
        return NULL;
    }
    return e;
}

int store_put(struct store *db, const char *key, uint32_t key_len, const char *value, uint32_t value_len,
              int64_t expires_at)
{
    if (key_len == 0 || key_len > MAX_KEY || value_len > MAX_VALUE)
        return STORE_TOO_BIG;
    struct record r = { RECORD_PUT, key_len, value_len, expires_at, key, value };
    off_t offset;
    if (append(db, &r, &offset) != 0)
        return STORE_IO_ERROR;
    index_record(db, &r, offset, record_size(key_len, value_len));
    maybe_auto_compact(db);
    return 0;
}

static int read_value(struct store *db, const struct entry *e, char **value)
{
    *value = malloc(e->value_len + 1);
    if (*value == NULL)
        return -1;
    if (e->value_len && pread_all(db->fd, *value, e->value_len, e->value_offset) != 0) {
        free(*value);
        *value = NULL;
        return -1;
    }
    (*value)[e->value_len] = '\0';
    return 0;
}

int store_get(struct store *db, const char *key, uint32_t key_len, char **value, uint32_t *value_len)
{
    *value = NULL;
    struct entry *e = lookup(db, key, key_len);
    if (e == NULL)
        return 0;
    if (read_value(db, e, value) != 0)
        return STORE_IO_ERROR;
    *value_len = e->value_len;
    return 1;
}

int store_delete(struct store *db, const char *key, uint32_t key_len)
{
    if (lookup(db, key, key_len) == NULL)
        return 0;
    struct record r = { RECORD_DELETE, key_len, 0, 0, key, NULL };
    off_t offset;
    if (append(db, &r, &offset) != 0)
        return STORE_IO_ERROR;
    unindex(db, key, key_len);
    maybe_auto_compact(db);
    return 1;
}

int store_expire(struct store *db, const char *key, uint32_t key_len, int64_t seconds)
{
    struct entry *e = lookup(db, key, key_len);
    if (e == NULL)
        return 0;
    if (seconds <= 0)
        return store_delete(db, key, key_len) < 0 ? STORE_IO_ERROR : 1;
    char *value;
    if (read_value(db, e, &value) != 0)
        return STORE_IO_ERROR;
    int rc = store_put(db, key, key_len, value, e->value_len, store_now(db) + seconds);
    free(value);
    return rc == 0 ? 1 : rc;
}

long long store_ttl(struct store *db, const char *key, uint32_t key_len)
{
    struct entry *e = lookup(db, key, key_len);
    if (e == NULL)
        return -2;
    if (e->expires_at == 0)
        return -1;
    return e->expires_at - store_now(db);
}

static int parse_integer(const char *s, uint32_t len, long long *out)
{
    if (len == 0 || len > 20)
        return -1;
    for (uint32_t i = 0; i < len; i++)
        if (!(s[i] >= '0' && s[i] <= '9') && !(i == 0 && s[i] == '-' && len > 1))
            return -1;
    errno = 0;
    char *end;
    long long value = strtoll(s, &end, 10);
    if (errno == ERANGE || end != s + len)
        return -1;
    *out = value;
    return 0;
}

int store_incr(struct store *db, const char *key, uint32_t key_len, long long by, long long *result)
{
    struct entry *e = lookup(db, key, key_len);
    long long current = 0;
    int64_t expires_at = 0;
    if (e != NULL) {
        char *value;
        if (read_value(db, e, &value) != 0)
            return STORE_IO_ERROR;
        int ok = parse_integer(value, e->value_len, &current);
        free(value);
        if (ok != 0)
            return STORE_NOT_INTEGER;
        expires_at = e->expires_at;
    }
    long long next;
    if (__builtin_add_overflow(current, by, &next))
        return STORE_OVERFLOW;
    char text[32];
    int n = snprintf(text, sizeof(text), "%lld", next);
    int rc = store_put(db, key, key_len, text, n, expires_at);
    if (rc == 0)
        *result = next;
    return rc;
}

struct entry_list {
    struct entry **items;
    size_t         count;
    size_t         cap;
    const char    *prefix;
    uint32_t       prefix_len;
    time_t         now;
};

static void collect(struct entry *e, void *ctx)
{
    struct entry_list *list = ctx;
    if (list->prefix_len && (e->key_len < list->prefix_len || memcmp(e->key, list->prefix, list->prefix_len) != 0))
        return;
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 64;
        struct entry **grown = realloc(list->items, list->cap * sizeof(*grown));
        if (grown == NULL) {
            perror("simplekv: out of memory");
            exit(EXIT_FAILURE);
        }
        list->items = grown;
    }
    list->items[list->count++] = e;
}

static void purge_expired(struct store *db)
{
    struct entry_list all = { 0 };
    index_each(db->ix, collect, &all);
    time_t now = store_now(db);
    for (size_t i = 0; i < all.count; i++) {
        struct entry *e = all.items[i];
        if (e->expires_at != 0 && e->expires_at <= now)
            unindex(db, e->key, e->key_len);
    }
    free(all.items);
}

static int compare_entries(const void *a, const void *b)
{
    const struct entry *x = *(struct entry * const *)a;
    const struct entry *y = *(struct entry * const *)b;
    uint32_t n = x->key_len < y->key_len ? x->key_len : y->key_len;
    int cmp = memcmp(x->key, y->key, n);
    return cmp ? cmp : (x->key_len > y->key_len) - (x->key_len < y->key_len);
}

int store_keys(struct store *db, const char *prefix, uint32_t prefix_len, char ***keys, uint32_t **lens, size_t *count)
{
    purge_expired(db);
    struct entry_list list = { .prefix = prefix, .prefix_len = prefix ? prefix_len : 0 };
    index_each(db->ix, collect, &list);
    if (list.count > 1)
        qsort(list.items, list.count, sizeof(*list.items), compare_entries);
    *keys = calloc(list.count ? list.count : 1, sizeof(**keys));
    *lens = calloc(list.count ? list.count : 1, sizeof(**lens));
    if (*keys == NULL || *lens == NULL) {
        free(*keys);
        free(*lens);
        free(list.items);
        return STORE_IO_ERROR;
    }
    for (size_t i = 0; i < list.count; i++) {
        (*keys)[i] = malloc(list.items[i]->key_len + 1);
        if ((*keys)[i] == NULL) {
            perror("simplekv: out of memory");
            exit(EXIT_FAILURE);
        }
        memcpy((*keys)[i], list.items[i]->key, list.items[i]->key_len);
        (*keys)[i][list.items[i]->key_len] = '\0';
        (*lens)[i] = list.items[i]->key_len;
    }
    *count = list.count;
    free(list.items);
    return 0;
}

void store_free_keys(char **keys, uint32_t *lens, size_t count)
{
    for (size_t i = 0; i < count; i++)
        free(keys[i]);
    free(keys);
    free(lens);
}

size_t store_count(struct store *db)
{
    purge_expired(db);
    return index_count(db->ix);
}

static int sync_directory(const char *path)
{
    char *copy = strdup(path);
    if (copy == NULL)
        return -1;
    int dir = open(dirname(copy), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(copy);
    if (dir < 0)
        return -1;
    int rc = fsync(dir);
    close(dir);
    return rc;
}

int store_compact(struct store *db, off_t *reclaimed)
{
    purge_expired(db);
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.compact", db->path) >= (int)sizeof(tmp))
        return STORE_IO_ERROR;
    int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return STORE_IO_ERROR;

    struct entry_list list = { 0 };
    index_each(db->ix, collect, &list);
    off_t *offsets = calloc(list.count ? list.count : 1, sizeof(*offsets));
    unsigned char header[FILE_HEADER];
    make_header(header);
    off_t end = FILE_HEADER;
    int ok = offsets != NULL && flock(fd, LOCK_EX | LOCK_NB) == 0 && pwrite_all(fd, header, FILE_HEADER, 0) == 0;

    for (size_t i = 0; ok && i < list.count; i++) {
        struct entry *e = list.items[i];
        char *value;
        if (read_value(db, e, &value) != 0) {
            ok = 0;
            break;
        }
        struct record r = { RECORD_PUT, e->key_len, e->value_len, e->expires_at, e->key, value };
        size_t total = record_size(e->key_len, e->value_len);
        unsigned char *buf = malloc(total);
        ok = buf != NULL;
        if (ok) {
            record_encode(&r, buf);
            ok = pwrite_all(fd, buf, total, end) == 0;
        }
        free(buf);
        free(value);
        offsets[i] = end;
        end += total;
    }
    ok = ok && fsync(fd) == 0 && rename(tmp, db->path) == 0;
    if (!ok) {
        close(fd);
        unlink(tmp);
        free(offsets);
        free(list.items);
        return STORE_IO_ERROR;
    }
    if (sync_directory(db->path) != 0)
        perror("simplekv: fsync of the database directory");

    close(db->fd);
    db->fd = fd;
    for (size_t i = 0; i < list.count; i++)
        list.items[i]->value_offset = offsets[i] + RECORD_HEADER + list.items[i]->key_len;
    *reclaimed = db->end - end;
    db->end = end;
    db->live_bytes = end - FILE_HEADER;
    db->records = list.count;
    db->unsynced = 0;
    db->last_compaction = store_now(db);
    free(offsets);
    free(list.items);
    return 0;
}

off_t store_take_auto_compaction(struct store *db)
{
    off_t reclaimed = db->auto_compacted;
    db->auto_compacted = 0;
    return reclaimed;
}

void store_stats(struct store *db, struct store_stats *stats)
{
    purge_expired(db);
    stats->keys = index_count(db->ix);
    stats->records = db->records;
    stats->file_size = db->end;
    stats->live_bytes = db->live_bytes;
    stats->dead_bytes = db->end - FILE_HEADER - db->live_bytes;
    stats->last_compaction = db->last_compaction;
    stats->sync = db->sync;
}

const char *store_recovery_note(struct store *db)
{
    return db->note[0] ? db->note : NULL;
}

void store_close(struct store *db)
{
    if (db == NULL)
        return;
    if (db->fd >= 0) {
        if (db->sync != SYNC_NEVER && fsync(db->fd) != 0)
            perror("simplekv: fsync");
        close(db->fd);
    }
    index_free(db->ix);
    free(db->path);
    free(db);
}
