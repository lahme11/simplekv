#include "simplekv.h"

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void report(const char *name, int n, double elapsed)
{
    printf("  %-8s %8d ops in %7.3f s = %10.0f ops/s\n", name, n, elapsed, n / elapsed);
}

int main(int argc, char *argv[])
{
    int n = 20000, opt;
    enum sync_mode sync = SYNC_NEVER;
    const char *mode = "never";

    while ((opt = getopt(argc, argv, "n:s:")) != -1) {
        if (opt == 'n')
            n = atoi(optarg);
        else if (opt == 's' && strcmp(optarg, "always") == 0)
            sync = SYNC_ALWAYS, mode = "always";
        else if (opt == 's' && strcmp(optarg, "batch") == 0)
            sync = SYNC_BATCH, mode = "batch";
        else if (opt == 's' && strcmp(optarg, "never") == 0)
            sync = SYNC_NEVER, mode = "never";
        else {
            fprintf(stderr, "Usage: %s [-n count] [-s always|batch|never]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (n <= 0) {
        fprintf(stderr, "bench: count must be positive\n");
        return EXIT_FAILURE;
    }

    char dir[] = "/tmp/skv-bench.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }
    char path[64], err[200];
    snprintf(path, sizeof(path), "%s/bench.skv", dir);
    struct store *db = store_open(path, sync, NULL, err, sizeof(err));
    if (db == NULL) {
        fprintf(stderr, "bench: %s\n", err);
        return EXIT_FAILURE;
    }

    srand(42);
    int *ids = malloc(n * sizeof(*ids));
    for (int i = 0; i < n; i++)
        ids[i] = rand();
    char key[32], value[100];
    memset(value, 'v', sizeof(value));

    printf("sync=%s, %d keys, 100-byte values\n", mode, n);
    double start = seconds();
    for (int i = 0; i < n; i++) {
        int len = snprintf(key, sizeof(key), "user:%d", ids[i]);
        store_put(db, key, len, value, sizeof(value), 0);
    }
    report("SET", n, seconds() - start);

    start = seconds();
    for (int i = 0; i < n; i++) {
        int len = snprintf(key, sizeof(key), "user:%d", ids[(i * 7919) % n]);
        char *out;
        uint32_t out_len;
        if (store_get(db, key, len, &out, &out_len) == 1)
            free(out);
    }
    report("GET", n, seconds() - start);

    start = seconds();
    for (int i = 0; i < n / 2; i++) {
        int len = snprintf(key, sizeof(key), "user:%d", ids[i]);
        store_delete(db, key, len);
    }
    report("DEL", n / 2, seconds() - start);

    struct store_stats st;
    store_stats(db, &st);
    off_t before = st.file_size, reclaimed = 0;
    start = seconds();
    store_compact(db, &reclaimed);
    double took = seconds() - start;
    store_stats(db, &st);
    printf("  COMPACT  %lld -> %lld bytes in %.3f s\n", (long long)before, (long long)st.file_size, took);

    store_close(db);
    unlink(path);
    rmdir(dir);
    free(ids);
    return EXIT_SUCCESS;
}
