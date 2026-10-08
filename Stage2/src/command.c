#include "simplekv.h"

typedef int (*handler)(struct store *db, char **argv, size_t *lens, int argc, FILE *out);

static int error(FILE *out, const char *message)
{
    fprintf(out, "ERR %s\n", message);
    return COMMAND_ERROR;
}

static int store_error(FILE *out, int code)
{
    switch (code) {
    case STORE_TOO_BIG:
        return error(out, "keys must be 1 to 1024 bytes and values at most 1048576 bytes");
    case STORE_NOT_INTEGER:
        return error(out, "value is not an integer or out of range");
    case STORE_OVERFLOW:
        return error(out, "increment would overflow");
    default:
        return error(out, strerror(errno ? errno : EIO));
    }
}

static int parse_long(const char *s, size_t len, long long *value)
{
    if (len == 0 || len > 20)
        return -1;
    char *end;
    errno = 0;
    *value = strtoll(s, &end, 10);
    return errno == 0 && end == s + len && s[0] != ' ' && s[0] != '+' ? 0 : -1;
}

static int parse_seconds(const char *s, size_t len, long long *seconds)
{
    return parse_long(s, len, seconds) == 0 && *seconds > 0 && *seconds <= 3153600000LL ? 0 : -1;
}

static int integer(FILE *out, long long n)
{
    fprintf(out, "(integer) %lld\n", n);
    return COMMAND_OK;
}

static int cmd_set(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argc;
    int rc = store_put(db, argv[1], lens[1], argv[2], lens[2], 0);
    if (rc != 0)
        return store_error(out, rc);
    fputs("OK\n", out);
    return COMMAND_OK;
}

static int cmd_setex(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argc;
    long long seconds;
    if (parse_seconds(argv[2], lens[2], &seconds) != 0)
        return error(out, "invalid expire time");
    int rc = store_put(db, argv[1], lens[1], argv[3], lens[3], store_now(db) + seconds);
    if (rc != 0)
        return store_error(out, rc);
    fputs("OK\n", out);
    return COMMAND_OK;
}

static int cmd_get(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argc;
    char *value;
    uint32_t len;
    int found = store_get(db, argv[1], lens[1], &value, &len);
    if (found < 0)
        return store_error(out, found);
    if (found == 0) {
        fputs("(nil)\n", out);
        return COMMAND_OK;
    }
    print_quoted(out, value, len);
    fputc('\n', out);
    free(value);
    return COMMAND_OK;
}

static int cmd_del(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argc;
    int rc = store_delete(db, argv[1], lens[1]);
    return rc < 0 ? store_error(out, rc) : integer(out, rc);
}

static int cmd_exists(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argc;
    return integer(out, store_ttl(db, argv[1], lens[1]) != -2);
}

static int cmd_ttl(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argc;
    return integer(out, store_ttl(db, argv[1], lens[1]));
}

static int cmd_expire(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argc;
    long long seconds;
    if (parse_seconds(argv[2], lens[2], &seconds) != 0)
        return error(out, "invalid expire time");
    int rc = store_expire(db, argv[1], lens[1], seconds);
    return rc < 0 ? store_error(out, rc) : integer(out, rc);
}

static int cmd_incr(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    long long by = 1, result;
    if (argc == 3 && parse_long(argv[2], lens[2], &by) != 0)
        return error(out, "value is not an integer or out of range");
    int rc = store_incr(db, argv[1], lens[1], by, &result);
    return rc != 0 ? store_error(out, rc) : integer(out, result);
}

static int cmd_keys(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    char **keys;
    uint32_t *key_lens;
    size_t count;
    int rc = store_keys(db, argc == 2 ? argv[1] : NULL, argc == 2 ? lens[1] : 0, &keys, &key_lens, &count);
    if (rc != 0)
        return store_error(out, rc);
    if (count == 0)
        fputs("(empty)\n", out);
    for (size_t i = 0; i < count; i++) {
        fprintf(out, "%zu) ", i + 1);
        print_quoted(out, keys[i], key_lens[i]);
        fputc('\n', out);
    }
    store_free_keys(keys, key_lens, count);
    return COMMAND_OK;
}

static int cmd_count(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argv, (void)lens, (void)argc;
    return integer(out, (long long)store_count(db));
}

static int cmd_compact(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argv, (void)lens, (void)argc;
    off_t reclaimed;
    if (store_compact(db, &reclaimed) != 0)
        return store_error(out, STORE_IO_ERROR);
    fprintf(out, "OK (reclaimed %lld bytes)\n", (long long)reclaimed);
    return COMMAND_OK;
}

static int cmd_stats(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)argv, (void)lens, (void)argc;
    static const char *modes[] = { "always", "batch", "never" };
    struct store_stats st;
    store_stats(db, &st);
    char when[32] = "never";
    if (st.last_compaction) {
        struct tm tm;
        localtime_r(&st.last_compaction, &tm);
        strftime(when, sizeof(when), "%d/%m/%Y %H:%M:%S", &tm);
    }
    fprintf(out, "keys: %zu\nrecords: %zu\nfile_size: %lld\nlive_bytes: %lld\ndead_bytes: %lld\n"
                 "sync: %s\nlast_compaction: %s\n",
            st.keys, st.records, (long long)st.file_size, (long long)st.live_bytes, (long long)st.dead_bytes,
            modes[st.sync], when);
    return COMMAND_OK;
}

static int cmd_help(struct store *db, char **argv, size_t *lens, int argc, FILE *out)
{
    (void)db, (void)argv, (void)lens, (void)argc;
    fputs("SET key value          SETEX key seconds value   GET key\n"
          "DEL key                EXISTS key                KEYS [prefix]\n"
          "COUNT                  EXPIRE key seconds        TTL key\n"
          "INCR key [by]          COMPACT                   STATS\n"
          "HELP                   QUIT\n"
          "Quote keys or values with spaces: SET \"full name\" \"Aoife Byrne\"\n", out);
    return COMMAND_OK;
}

static const struct {
    const char *name;
    int         min_args;
    int         max_args;
    handler     run;
} COMMANDS[] = {
    { "set", 3, 3, cmd_set },       { "setex", 4, 4, cmd_setex },     { "get", 2, 2, cmd_get },
    { "del", 2, 2, cmd_del },       { "exists", 2, 2, cmd_exists },   { "ttl", 2, 2, cmd_ttl },
    { "expire", 3, 3, cmd_expire }, { "incr", 2, 3, cmd_incr },       { "keys", 1, 2, cmd_keys },
    { "count", 1, 1, cmd_count },   { "compact", 1, 1, cmd_compact }, { "stats", 1, 1, cmd_stats },
    { "help", 1, 1, cmd_help },
};

int run_command(struct store *db, int argc, char **argv, size_t *lens, FILE *out)
{
    if (strcasecmp(argv[0], "quit") == 0 || strcasecmp(argv[0], "exit") == 0)
        return COMMAND_QUIT;
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcasecmp(argv[0], COMMANDS[i].name) != 0)
            continue;
        if (argc < COMMANDS[i].min_args || argc > COMMANDS[i].max_args) {
            fprintf(out, "ERR wrong number of arguments for '%s'\n", COMMANDS[i].name);
            return COMMAND_ERROR;
        }
        return COMMANDS[i].run(db, argv, lens, argc, out);
    }
    fputs("ERR unknown command '", out);
    fwrite(argv[0], 1, lens[0], out);
    fputs("'\n", out);
    return COMMAND_ERROR;
}
