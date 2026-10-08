#include "simplekv.h"

struct index {
    struct entry **buckets;
    size_t         nbuckets;
    size_t         count;
};

static uint64_t hash_key(const char *key, uint32_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < len; i++) {
        h ^= (unsigned char)key[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void *must_calloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (p == NULL) {
        perror("simplekv: out of memory");
        exit(EXIT_FAILURE);
    }
    return p;
}

struct index *index_new(void)
{
    struct index *ix = must_calloc(1, sizeof(*ix));
    ix->nbuckets = INITIAL_BUCKETS;
    ix->buckets = must_calloc(ix->nbuckets, sizeof(*ix->buckets));
    return ix;
}

void index_free(struct index *ix)
{
    if (ix == NULL)
        return;
    for (size_t b = 0; b < ix->nbuckets; b++) {
        struct entry *e = ix->buckets[b];
        while (e != NULL) {
            struct entry *next = e->next;
            free(e->key);
            free(e);
            e = next;
        }
    }
    free(ix->buckets);
    free(ix);
}

struct entry *index_get(struct index *ix, const char *key, uint32_t len)
{
    for (struct entry *e = ix->buckets[hash_key(key, len) % ix->nbuckets]; e != NULL; e = e->next) {
        if (e->key_len == len && memcmp(e->key, key, len) == 0)
            return e;
    }
    return NULL;
}

static void grow(struct index *ix)
{
    size_t nbuckets = ix->nbuckets * 2;
    struct entry **buckets = must_calloc(nbuckets, sizeof(*buckets));
    for (size_t b = 0; b < ix->nbuckets; b++) {
        struct entry *e = ix->buckets[b];
        while (e != NULL) {
            struct entry *next = e->next;
            size_t slot = hash_key(e->key, e->key_len) % nbuckets;
            e->next = buckets[slot];
            buckets[slot] = e;
            e = next;
        }
    }
    free(ix->buckets);
    ix->buckets = buckets;
    ix->nbuckets = nbuckets;
}

struct entry *index_put(struct index *ix, const char *key, uint32_t len)
{
    struct entry *e = index_get(ix, key, len);
    if (e != NULL)
        return e;
    if ((ix->count + 1) * 4 > ix->nbuckets * 3)
        grow(ix);
    e = must_calloc(1, sizeof(*e));
    e->key = must_calloc(len + 1, 1);
    memcpy(e->key, key, len);
    e->key_len = len;
    size_t slot = hash_key(key, len) % ix->nbuckets;
    e->next = ix->buckets[slot];
    ix->buckets[slot] = e;
    ix->count++;
    return e;
}

int index_remove(struct index *ix, const char *key, uint32_t len)
{
    struct entry **link = &ix->buckets[hash_key(key, len) % ix->nbuckets];
    while (*link != NULL) {
        struct entry *e = *link;
        if (e->key_len == len && memcmp(e->key, key, len) == 0) {
            *link = e->next;
            free(e->key);
            free(e);
            ix->count--;
            return 1;
        }
        link = &e->next;
    }
    return 0;
}

size_t index_count(const struct index *ix)
{
    return ix->count;
}

size_t index_buckets(const struct index *ix)
{
    return ix->nbuckets;
}

void index_each(struct index *ix, void (*fn)(struct entry *, void *), void *ctx)
{
    for (size_t b = 0; b < ix->nbuckets; b++)
        for (struct entry *e = ix->buckets[b]; e != NULL; e = e->next)
            fn(e, ctx);
}
