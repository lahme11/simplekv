#include "simplekv.h"

static struct item *buckets[BUCKETS];
static size_t       item_count = 0;
static FILE        *log_file = NULL;

static size_t bucket_of(const char *key, size_t len)
{
    unsigned int hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (unsigned char)key[i];
        hash *= 16777619u;
    }
    return hash % BUCKETS;
}

static char *copy_bytes(const char *data, size_t len)
{
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        perror("simplekv: malloc");
        exit(EXIT_FAILURE);
    }
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

struct item *table_get(const char *key, size_t key_len)
{
    for (struct item *it = buckets[bucket_of(key, key_len)]; it != NULL; it = it->next) {
        if (it->key_len == key_len && memcmp(it->key, key, key_len) == 0)
            return it;
    }
    return NULL;
}

void table_set(const char *key, size_t key_len, const char *value, size_t value_len)
{
    struct item *it = table_get(key, key_len);
    if (it != NULL) {
        free(it->value);
        it->value = copy_bytes(value, value_len);
        it->value_len = value_len;
        return;
    }
    size_t bucket = bucket_of(key, key_len);
    it = malloc(sizeof(*it));
    if (it == NULL) {
        perror("simplekv: malloc");
        exit(EXIT_FAILURE);
    }
    it->key = copy_bytes(key, key_len);
    it->key_len = key_len;
    it->value = copy_bytes(value, value_len);
    it->value_len = value_len;
    it->next = buckets[bucket];
    buckets[bucket] = it;
    item_count++;
}

int table_delete(const char *key, size_t key_len)
{
    struct item **link = &buckets[bucket_of(key, key_len)];
    while (*link != NULL) {
        struct item *it = *link;
        if (it->key_len == key_len && memcmp(it->key, key, key_len) == 0) {
            *link = it->next;
            free(it->key);
            free(it->value);
            free(it);
            item_count--;
            return 1;
        }
        link = &it->next;
    }
    return 0;
}

size_t table_count(void)
{
    return item_count;
}

static int compare_items(const void *a, const void *b)
{
    const struct item *x = *(struct item * const *)a;
    const struct item *y = *(struct item * const *)b;
    size_t n = x->key_len < y->key_len ? x->key_len : y->key_len;
    int cmp = memcmp(x->key, y->key, n);
    if (cmp != 0)
        return cmp;
    return (x->key_len > y->key_len) - (x->key_len < y->key_len);
}

size_t table_keys(struct item ***items_out)
{
    struct item **items = malloc((item_count ? item_count : 1) * sizeof(*items));
    if (items == NULL) {
        perror("simplekv: malloc");
        exit(EXIT_FAILURE);
    }
    size_t n = 0;
    for (size_t b = 0; b < BUCKETS; b++)
        for (struct item *it = buckets[b]; it != NULL; it = it->next)
            items[n++] = it;
    if (n > 1)
        qsort(items, n, sizeof(*items), compare_items);
    *items_out = items;
    return n;
}

void table_free(void)
{
    for (size_t b = 0; b < BUCKETS; b++) {
        while (buckets[b] != NULL) {
            struct item *it = buckets[b];
            buckets[b] = it->next;
            free(it->key);
            free(it->value);
            free(it);
        }
    }
    item_count = 0;
}

static char *read_field(FILE *in, size_t *len)
{
    if (fscanf(in, " %zu", len) != 1 || fgetc(in) != ' ' || *len > 64 * 1024 * 1024)
        return NULL;
    char *data = malloc(*len + 1);
    if (data == NULL)
        return NULL;
    if (fread(data, 1, *len, in) != *len) {
        free(data);
        return NULL;
    }
    data[*len] = '\0';
    return data;
}

static void replay(FILE *in, int fd)
{
    long good = 0;
    int type;
    while ((type = fgetc(in)) != EOF) {
        size_t key_len, value_len = 0;
        char *key = NULL, *value = NULL;
        int ok = 0;

        if (type == 'S' || type == 'D') {
            key = read_field(in, &key_len);
            if (key != NULL && type == 'S')
                value = read_field(in, &value_len);
            ok = key != NULL && (type == 'D' || value != NULL) && fgetc(in) == '\n';
        }
        if (!ok) {
            free(key);
            free(value);
            fprintf(stderr, "simplekv: ignored a damaged entry at the end of the log (offset %ld)\n", good);
            if (ftruncate(fd, good) != 0)
                perror("simplekv: ftruncate");
            break;
        }
        if (type == 'S')
            table_set(key, key_len, value, value_len);
        else
            table_delete(key, key_len);
        free(key);
        free(value);
        good = ftell(in);
    }
    fseek(in, 0, SEEK_END);
}

int log_open(const char *path)
{
    log_file = fopen(path, "a+");
    if (log_file == NULL) {
        perror("simplekv: cannot open log");
        return -1;
    }
    rewind(log_file);
    replay(log_file, fileno(log_file));
    return 0;
}

void log_close(void)
{
    if (log_file != NULL)
        fclose(log_file);
    log_file = NULL;
}

static void write_field(const char *data, size_t len)
{
    fprintf(log_file, " %zu ", len);
    fwrite(data, 1, len, log_file);
}

int log_set(const char *key, size_t key_len, const char *value, size_t value_len)
{
    fputc('S', log_file);
    write_field(key, key_len);
    write_field(value, value_len);
    fputc('\n', log_file);
    return fflush(log_file) == 0 ? 0 : -1;
}

int log_delete(const char *key, size_t key_len)
{
    fputc('D', log_file);
    write_field(key, key_len);
    fputc('\n', log_file);
    return fflush(log_file) == 0 ? 0 : -1;
}
