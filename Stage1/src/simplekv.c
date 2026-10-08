#include "simplekv.h"

static int tokenise(char *line, char **args, size_t *lens, const char **error)
{
    int argc = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == '\0')
            break;
        if (argc == MAX_ARGS) {
            *error = "too many arguments";
            return -1;
        }
        char *out = p;
        args[argc] = out;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
                    p++;
                *out++ = *p++;
            }
            if (*p != '"') {
                *error = "unterminated quote";
                return -1;
            }
            p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
                *out++ = *p++;
        }
        lens[argc] = out - args[argc];
        argc++;
        if (*p)
            p++;
        *out = '\0';
    }
    return argc;
}

static void print_value(const char *s, size_t len)
{
    int plain = len > 0;
    for (size_t i = 0; i < len; i++)
        if (s[i] == ' ' || s[i] == '"' || s[i] == '\\' || (unsigned char)s[i] < 32)
            plain = 0;
    if (plain) {
        fwrite(s, 1, len, stdout);
        putchar('\n');
        return;
    }
    putchar('"');
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"' || s[i] == '\\')
            putchar('\\');
        putchar(s[i]);
    }
    puts("\"");
}

static int run(int argc, char **args, size_t *lens)
{
    const char *cmd = args[0];

    if (strcasecmp(cmd, "QUIT") == 0 || strcasecmp(cmd, "EXIT") == 0)
        return 0;
    if (strcasecmp(cmd, "SET") == 0 && argc == 3) {
        table_set(args[1], lens[1], args[2], lens[2]);
        puts(log_set(args[1], lens[1], args[2], lens[2]) == 0 ? "OK" : "ERR could not write the log");
    } else if (strcasecmp(cmd, "GET") == 0 && argc == 2) {
        struct item *it = table_get(args[1], lens[1]);
        if (it == NULL)
            puts("(nil)");
        else
            print_value(it->value, it->value_len);
    } else if (strcasecmp(cmd, "DEL") == 0 && argc == 2) {
        int removed = table_delete(args[1], lens[1]);
        if (removed)
            log_delete(args[1], lens[1]);
        printf("(integer) %d\n", removed);
    } else if (strcasecmp(cmd, "EXISTS") == 0 && argc == 2) {
        printf("(integer) %d\n", table_get(args[1], lens[1]) != NULL);
    } else if (strcasecmp(cmd, "COUNT") == 0 && argc == 1) {
        printf("(integer) %zu\n", table_count());
    } else if (strcasecmp(cmd, "KEYS") == 0 && argc <= 2) {
        struct item **items;
        size_t n = table_keys(&items), shown = 0;
        for (size_t i = 0; i < n; i++) {
            if (argc == 2 && (items[i]->key_len < lens[1] || memcmp(items[i]->key, args[1], lens[1]) != 0))
                continue;
            printf("%zu) ", ++shown);
            print_value(items[i]->key, items[i]->key_len);
        }
        if (shown == 0)
            puts("(empty)");
        free(items);
    } else if (strcasecmp(cmd, "HELP") == 0) {
        puts("SET key value | GET key | DEL key | EXISTS key | KEYS [prefix] | COUNT | HELP | QUIT");
    } else if (strcasecmp(cmd, "SET") == 0 || strcasecmp(cmd, "GET") == 0 || strcasecmp(cmd, "DEL") == 0 ||
               strcasecmp(cmd, "EXISTS") == 0 || strcasecmp(cmd, "COUNT") == 0 || strcasecmp(cmd, "KEYS") == 0) {
        printf("ERR wrong number of arguments for '%s'\n", cmd);
    } else {
        printf("ERR unknown command '%s'\n", cmd);
    }
    return 1;
}

int main(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [logfile]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *path = argc == 2 ? argv[1] : DEFAULT_LOG;
    if (log_open(path) != 0)
        return EXIT_FAILURE;

    int interactive = isatty(STDIN_FILENO);
    if (interactive)
        printf("simplekv: %zu key(s) loaded from %s. Type HELP for commands.\n", table_count(), path);

    char line[LINE_BUFSIZE];
    while (1) {
        if (interactive) {
            printf("simplekv> ");
            fflush(stdout);
        }
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        char *args[MAX_ARGS];
        size_t lens[MAX_ARGS];
        const char *error = NULL;
        int count = tokenise(line, args, lens, &error);
        if (count < 0)
            printf("ERR %s\n", error);
        else if (count > 0 && !run(count, args, lens))
            break;
        fflush(stdout);
    }

    log_close();
    table_free();
    return EXIT_SUCCESS;
}
