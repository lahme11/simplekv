#include "simplekv.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-f file] [-s always|batch|never] [-q] [-h]\n"
            "  -f file   database file (default %s)\n"
            "  -s mode   when to fsync: always (default), batch (every %d writes) or never\n"
            "  -q        no banner or prompt\n"
            "  -h        show this message\n",
            prog, DEFAULT_DB, BATCH_SYNC_EVERY);
}

static int parse_sync(const char *text, enum sync_mode *mode)
{
    if (strcmp(text, "always") == 0)
        *mode = SYNC_ALWAYS;
    else if (strcmp(text, "batch") == 0)
        *mode = SYNC_BATCH;
    else if (strcmp(text, "never") == 0)
        *mode = SYNC_NEVER;
    else
        return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    const char *path = DEFAULT_DB;
    enum sync_mode sync = SYNC_ALWAYS;
    int quiet = 0, opt;

    while ((opt = getopt(argc, argv, "f:s:qh")) != -1) {
        switch (opt) {
        case 'f':
            path = optarg;
            break;
        case 's':
            if (parse_sync(optarg, &sync) != 0) {
                fprintf(stderr, "simplekv: unknown sync mode '%s'\n", optarg);
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            break;
        case 'q':
            quiet = 1;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    char err[256];
    struct store *db = store_open(path, sync, NULL, err, sizeof(err));
    if (db == NULL) {
        fprintf(stderr, "simplekv: %s\n", err);
        return EXIT_FAILURE;
    }
    if (store_recovery_note(db))
        fprintf(stderr, "simplekv: %s\n", store_recovery_note(db));

    int interactive = isatty(STDIN_FILENO) && !quiet;
    if (interactive)
        printf("simplekv: %zu key(s) in %s. Type HELP for commands.\n", store_count(db), path);

    char *line = NULL;
    size_t capacity = 0;
    int failed = 0;
    while (1) {
        if (interactive) {
            fputs("simplekv> ", stdout);
            fflush(stdout);
        }
        ssize_t len = getline(&line, &capacity, stdin);
        if (len < 0)
            break;
        if (len > LINE_MAX_LEN) {
            puts("ERR line too long");
            failed = 1;
            continue;
        }
        char *args[MAX_ARGS];
        size_t lens[MAX_ARGS];
        const char *problem = NULL;
        int count = tokenize(line, args, lens, MAX_ARGS, &problem);
        int status = COMMAND_OK;
        if (count < 0) {
            printf("ERR %s\n", problem);
            status = COMMAND_ERROR;
        } else if (count > 0) {
            status = run_command(db, count, args, lens, stdout);
        }
        off_t reclaimed = store_take_auto_compaction(db);
        if (reclaimed > 0)
            fprintf(stderr, "simplekv: auto-compacted (reclaimed %lld bytes)\n", (long long)reclaimed);
        fflush(stdout);
        if (status == COMMAND_QUIT)
            break;
        if (status == COMMAND_ERROR)
            failed = 1;
    }

    free(line);
    store_close(db);
    return failed && !isatty(STDIN_FILENO) ? EXIT_FAILURE : EXIT_SUCCESS;
}
