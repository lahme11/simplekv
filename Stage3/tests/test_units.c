#include "simplekv.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static char *quoted(const char *s, size_t len)
{
    static char buf[256];
    FILE *mem = fmemopen(buf, sizeof(buf), "w");
    print_quoted(mem, s, len);
    fclose(mem);
    return buf;
}

static void test_crc(void)
{
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    CHECK(crc32("", 0) == 0);
}

static void test_record_codec(void)
{
    unsigned char buf[256];
    struct record in = { RECORD_PUT, 3, 5, 1700000000, "key", "value" };
    size_t n = record_encode(&in, buf);

    CHECK(n == RECORD_HEADER + 8);
    CHECK(n == record_size(3, 5));

    struct record out;
    uint32_t crc;
    CHECK(record_decode_header(buf, n, &out, &crc) == RECORD_OK);
    CHECK(out.type == RECORD_PUT && out.key_len == 3 && out.value_len == 5 && out.expires_at == 1700000000);
    CHECK(record_check(buf, n));

    buf[n - 1] ^= 0x40;
    CHECK(!record_check(buf, n));
    buf[n - 1] ^= 0x40;

    CHECK(record_decode_header(buf, RECORD_HEADER - 1, &out, &crc) == RECORD_SHORT);

    unsigned char bad[RECORD_HEADER];
    memcpy(bad, buf, RECORD_HEADER);
    put_u32(bad + 5, MAX_KEY + 1);
    CHECK(record_decode_header(bad, RECORD_HEADER, &out, &crc) == RECORD_BAD);

    memcpy(bad, buf, RECORD_HEADER);
    bad[4] = 9;
    CHECK(record_decode_header(bad, RECORD_HEADER, &out, &crc) == RECORD_BAD);

    unsigned char little[8];
    put_u32(little, 0x01020304u);
    CHECK(little[0] == 4 && little[3] == 1);
    put_i64(little, -2);
    CHECK(get_i64(little) == -2);
}

static void test_tokenize(void)
{
    char *argv[8];
    size_t lens[8];
    const char *error = NULL;
    char line[] = "SET \"a b\" \"c\\\"d\\n\"  plain\n";

    CHECK(tokenize(line, argv, lens, 8, &error) == 4);
    CHECK(strcmp(argv[0], "SET") == 0);
    CHECK(lens[1] == 3 && memcmp(argv[1], "a b", 3) == 0);
    CHECK(lens[2] == 4 && memcmp(argv[2], "c\"d\n", 4) == 0);
    CHECK(strcmp(argv[3], "plain") == 0);

    char empty[] = "SET k \"\"";
    CHECK(tokenize(empty, argv, lens, 8, &error) == 3);
    CHECK(lens[2] == 0);

    char open[] = "SET k \"oops";
    CHECK(tokenize(open, argv, lens, 8, &error) == -1);
    CHECK(strcmp(error, "unterminated quote") == 0);

    char many[] = "a b c d e f g h i";
    CHECK(tokenize(many, argv, lens, 8, &error) == -1);

    char blank[] = "   \n";
    CHECK(tokenize(blank, argv, lens, 8, &error) == 0);

    char hex[] = "\"a\\x01b\"";
    CHECK(tokenize(hex, argv, lens, 8, &error) == 1 && lens[0] == 3 && argv[0][1] == 1);

    char tab[] = "\"t\\tx\"";
    CHECK(tokenize(tab, argv, lens, 8, &error) == 1 && lens[0] == 3 && argv[0][1] == '\t');
}

static void test_print_quoted(void)
{
    CHECK(strcmp(quoted("plain", 5), "plain") == 0);
    CHECK(strcmp(quoted("x y", 3), "\"x y\"") == 0);
    CHECK(strcmp(quoted("", 0), "\"\"") == 0);
    CHECK(strcmp(quoted("a\nb", 3), "\"a\\nb\"") == 0);
    CHECK(strcmp(quoted("say \"hi\"", 8), "\"say \\\"hi\\\"\"") == 0);
    CHECK(strcmp(quoted("\x01", 1), "\"\\x01\"") == 0);
}

static void count_entry(struct entry *e, void *ctx)
{
    (void)e;
    (*(size_t *)ctx)++;
}

static void test_index(void)
{
    struct index *ix = index_new();
    CHECK(index_count(ix) == 0);
    CHECK(index_buckets(ix) == INITIAL_BUCKETS);

    struct entry *e = index_put(ix, "apple", 5);
    e->value_offset = 100;
    CHECK(index_count(ix) == 1);
    CHECK(index_get(ix, "apple", 5)->value_offset == 100);
    CHECK(index_get(ix, "apples", 6) == NULL);

    CHECK(index_put(ix, "apple", 5) == e);
    CHECK(index_count(ix) == 1);

    CHECK(index_put(ix, "a\0b", 3) != index_put(ix, "a\0c", 3));
    CHECK(index_count(ix) == 3);

    CHECK(index_remove(ix, "apple", 5) == 1);
    CHECK(index_remove(ix, "apple", 5) == 0);
    CHECK(index_get(ix, "apple", 5) == NULL);

    char key[32];
    for (int i = 0; i < 10000; i++) {
        int n = snprintf(key, sizeof(key), "key-%d", i);
        index_put(ix, key, n)->value_offset = i;
    }
    CHECK(index_count(ix) == 10002);
    CHECK(index_buckets(ix) >= 16384);
    int found = 1;
    for (int i = 0; i < 10000; i++) {
        int n = snprintf(key, sizeof(key), "key-%d", i);
        struct entry *got = index_get(ix, key, n);
        found = found && got != NULL && got->value_offset == i;
    }
    CHECK(found);

    size_t visited = 0;
    index_each(ix, count_entry, &visited);
    CHECK(visited == 10002);
    index_free(ix);
}

static time_t fake_now = 1800000000;

static time_t fake_clock(void)
{
    return fake_now;
}

static char tmp_dir[64];

static const char *tmp_path(const char *name)
{
    static char path[128];
    snprintf(path, sizeof(path), "%s/%s", tmp_dir, name);
    return path;
}

static struct store *open_db(const char *name)
{
    char err[200];
    struct store *db = store_open(tmp_path(name), SYNC_NEVER, fake_clock, err, sizeof(err));
    if (db == NULL)
        fprintf(stderr, "open %s failed: %s\n", name, err);
    return db;
}

static int has_value(struct store *db, const char *key, const char *expected)
{
    char *value = NULL;
    uint32_t len = 0;
    int found = store_get(db, key, strlen(key), &value, &len);
    int ok = found == 1 && len == strlen(expected) && memcmp(value, expected, len) == 0;
    free(value);
    return ok;
}

static int missing(struct store *db, const char *key)
{
    char *value = NULL;
    uint32_t len;
    int found = store_get(db, key, strlen(key), &value, &len);
    free(value);
    return found == 0;
}

static off_t file_size(const char *name)
{
    struct stat st;
    return stat(tmp_path(name), &st) == 0 ? st.st_size : -1;
}

static void test_persistence(void)
{
    struct store *db = open_db("persist.skv");
    CHECK(db != NULL);
    CHECK(store_put(db, "name", 4, "Aoife", 5, 0) == 0);
    CHECK(store_put(db, "city", 4, "Cork", 4, 0) == 0);
    CHECK(store_put(db, "name", 4, "Niamh", 5, 0) == 0);
    CHECK(store_put(db, "empty", 5, "", 0, 0) == 0);
    CHECK(store_delete(db, "city", 4) == 1);
    CHECK(store_delete(db, "city", 4) == 0);
    CHECK(store_recovery_note(db) == NULL);
    store_close(db);

    db = open_db("persist.skv");
    CHECK(has_value(db, "name", "Niamh"));
    CHECK(missing(db, "city"));
    CHECK(has_value(db, "empty", ""));
    CHECK(store_recovery_note(db) == NULL);

    struct store_stats st;
    store_stats(db, &st);
    CHECK(st.keys == 2);
    CHECK(st.records == 5);
    CHECK(st.live_bytes + st.dead_bytes + FILE_HEADER == st.file_size);
    store_close(db);
}

static void test_truncated_tail(void)
{
    struct store *db = open_db("torn.skv");
    store_put(db, "a", 1, "first", 5, 0);
    store_put(db, "b", 1, "second", 6, 0);
    store_close(db);
    off_t size = file_size("torn.skv");
    CHECK(truncate(tmp_path("torn.skv"), size - 5) == 0);

    db = open_db("torn.skv");
    CHECK(has_value(db, "a", "first"));
    CHECK(missing(db, "b"));
    CHECK(store_recovery_note(db) != NULL && strstr(store_recovery_note(db), "removed") != NULL);
    store_put(db, "c", 1, "third", 5, 0);
    store_close(db);

    db = open_db("torn.skv");
    CHECK(has_value(db, "c", "third"));
    CHECK(store_recovery_note(db) == NULL);
    store_close(db);
}

static void flip_byte(const char *name, off_t offset)
{
    int fd = open(tmp_path(name), O_RDWR);
    unsigned char b;
    if (pread(fd, &b, 1, offset) == 1) {
        b ^= 0x20;
        if (pwrite(fd, &b, 1, offset) != 1)
            perror("pwrite");
    }
    close(fd);
}

static void test_corruption(void)
{
    struct store *db = open_db("flip.skv");
    store_put(db, "a", 1, "aaaa", 4, 0);
    store_put(db, "b", 1, "bbbb", 4, 0);
    store_close(db);
    flip_byte("flip.skv", file_size("flip.skv") - 1);

    db = open_db("flip.skv");
    CHECK(has_value(db, "a", "aaaa"));
    CHECK(missing(db, "b"));
    CHECK(store_recovery_note(db) != NULL);
    store_close(db);

    db = open_db("flip2.skv");
    store_put(db, "a", 1, "aaaa", 4, 0);
    store_put(db, "b", 1, "bbbb", 4, 0);
    store_close(db);
    flip_byte("flip2.skv", FILE_HEADER + RECORD_HEADER);

    db = open_db("flip2.skv");
    CHECK(missing(db, "a"));
    CHECK(missing(db, "b"));
    CHECK(file_size("flip2.skv") == FILE_HEADER);
    store_close(db);
}

static void test_refusals(void)
{
    FILE *f = fopen(tmp_path("other.skv"), "w");
    fputs("not a database at all", f);
    fclose(f);
    char err[200] = "";
    CHECK(store_open(tmp_path("other.skv"), SYNC_NEVER, fake_clock, err, sizeof(err)) == NULL);
    CHECK(strstr(err, "not a SimpleKV file") != NULL);

    struct store *first = open_db("locked.skv");
    CHECK(first != NULL);
    CHECK(store_open(tmp_path("locked.skv"), SYNC_NEVER, fake_clock, err, sizeof(err)) == NULL);
    CHECK(strstr(err, "locked") != NULL);
    store_close(first);
    struct store *again = open_db("locked.skv");
    CHECK(again != NULL);
    store_close(again);

    struct store *db = open_db("limits.skv");
    char big[MAX_KEY + 1];
    memset(big, 'k', sizeof(big));
    CHECK(store_put(db, big, sizeof(big), "v", 1, 0) == STORE_TOO_BIG);
    CHECK(store_put(db, "", 0, "v", 1, 0) == STORE_TOO_BIG);
    store_close(db);
}

static void test_ttl(void)
{
    struct store *db = open_db("ttl.skv");
    store_put(db, "session", 7, "abc", 3, fake_now + 10);
    store_put(db, "forever", 7, "x", 1, 0);
    CHECK(store_ttl(db, "session", 7) == 10);
    CHECK(store_ttl(db, "forever", 7) == -1);
    CHECK(store_ttl(db, "nobody", 6) == -2);

    fake_now += 11;
    CHECK(missing(db, "session"));
    CHECK(store_ttl(db, "session", 7) == -2);
    CHECK(store_expire(db, "session", 7, 5) == 0);

    CHECK(store_expire(db, "forever", 7, 30) == 1);
    CHECK(store_ttl(db, "forever", 7) == 30);
    store_close(db);

    db = open_db("ttl.skv");
    CHECK(store_ttl(db, "forever", 7) == 30);
    CHECK(missing(db, "session"));
    store_close(db);
}

static void test_incr(void)
{
    struct store *db = open_db("incr.skv");
    long long result = 0;
    CHECK(store_incr(db, "hits", 4, 1, &result) == 0 && result == 1);
    CHECK(store_incr(db, "hits", 4, 41, &result) == 0 && result == 42);
    CHECK(has_value(db, "hits", "42"));
    CHECK(store_incr(db, "hits", 4, -50, &result) == 0 && result == -8);

    store_put(db, "word", 4, "abc", 3, 0);
    CHECK(store_incr(db, "word", 4, 1, &result) == STORE_NOT_INTEGER);
    store_put(db, "spaced", 6, " 12", 3, 0);
    CHECK(store_incr(db, "spaced", 6, 1, &result) == STORE_NOT_INTEGER);

    char max[32];
    int n = snprintf(max, sizeof(max), "%lld", LLONG_MAX);
    store_put(db, "big", 3, max, n, 0);
    CHECK(store_incr(db, "big", 3, 1, &result) == STORE_OVERFLOW);
    CHECK(has_value(db, "big", max));
    store_close(db);
}

static void test_keys(void)
{
    struct store *db = open_db("keys.skv");
    store_put(db, "user:2", 6, "b", 1, 0);
    store_put(db, "user:10", 7, "c", 1, 0);
    store_put(db, "user:1", 6, "a", 1, 0);
    store_put(db, "order:1", 7, "x", 1, 0);
    store_put(db, "user:old", 8, "y", 1, fake_now - 1);

    char **keys;
    uint32_t *lens;
    size_t n;
    CHECK(store_keys(db, "user:", 5, &keys, &lens, &n) == 0);
    CHECK(n == 3);
    CHECK(n == 3 && strcmp(keys[0], "user:1") == 0 && strcmp(keys[1], "user:10") == 0 && strcmp(keys[2], "user:2") == 0);
    store_free_keys(keys, lens, n);

    CHECK(store_keys(db, NULL, 0, &keys, &lens, &n) == 0 && n == 4);
    store_free_keys(keys, lens, n);
    store_close(db);
}

static void test_compaction(void)
{
    struct store *db = open_db("compact.skv");
    char value[64];
    for (int i = 0; i < 100; i++) {
        int n = snprintf(value, sizeof(value), "version %d of the value", i);
        store_put(db, "counter", 7, value, n, 0);
    }
    store_put(db, "keep", 4, "me", 2, 0);
    store_put(db, "gone", 4, "soon", 4, 0);
    store_delete(db, "gone", 4);
    store_put(db, "brief", 5, "x", 1, fake_now + 1);
    fake_now += 2;
    off_t before = file_size("compact.skv");

    off_t reclaimed = 0;
    CHECK(store_compact(db, &reclaimed) == 0);
    off_t after = file_size("compact.skv");
    CHECK(after < before / 5);
    CHECK(reclaimed == before - after);
    CHECK(has_value(db, "counter", "version 99 of the value"));
    CHECK(has_value(db, "keep", "me"));
    CHECK(missing(db, "gone"));

    struct store_stats st;
    store_stats(db, &st);
    CHECK(st.dead_bytes == 0);
    CHECK(st.keys == 2 && st.records == 2);
    CHECK(st.last_compaction == fake_now);

    store_put(db, "after", 5, "compaction", 10, 0);
    store_close(db);
    CHECK(access(tmp_path("compact.skv.compact"), F_OK) != 0);

    db = open_db("compact.skv");
    CHECK(has_value(db, "counter", "version 99 of the value"));
    CHECK(has_value(db, "after", "compaction"));
    CHECK(store_recovery_note(db) == NULL);
    store_close(db);
}

static void test_failed_compaction(void)
{
    struct store *db = open_db("stuck.skv");
    store_put(db, "a", 1, "1", 1, 0);
    store_put(db, "a", 1, "2", 1, 0);
    CHECK(mkdir(tmp_path("stuck.skv.compact"), 0755) == 0);
    off_t before = file_size("stuck.skv");

    off_t reclaimed = 0;
    CHECK(store_compact(db, &reclaimed) == -1);
    CHECK(file_size("stuck.skv") == before);
    CHECK(has_value(db, "a", "2"));
    CHECK(store_put(db, "b", 1, "3", 1, 0) == 0);
    CHECK(has_value(db, "b", "3"));
    store_close(db);
    rmdir(tmp_path("stuck.skv.compact"));
}

int main(void)
{
    test_crc();
    test_record_codec();
    test_tokenize();
    test_print_quoted();
    test_index();

    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/skv-unit.XXXXXX");
    if (mkdtemp(tmp_dir) == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }
    test_persistence();
    test_truncated_tail();
    test_corruption();
    test_refusals();
    test_ttl();
    test_incr();
    test_keys();
    test_compaction();
    test_failed_compaction();
    char cleanup[128];
    snprintf(cleanup, sizeof(cleanup), "rm -r '%s'", tmp_dir);
    if (system(cleanup) != 0)
        fprintf(stderr, "could not remove %s\n", tmp_dir);

    if (failures > 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return EXIT_FAILURE;
    }
    printf("all %d checks passed\n", checks);
    return EXIT_SUCCESS;
}
