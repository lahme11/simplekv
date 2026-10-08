#include "simplekv.h"

static int unescape(char c)
{
    switch (c) {
    case '"':  return '"';
    case '\\': return '\\';
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    default:   return -1;
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tokenize(char *line, char **argv, size_t *lens, int max, const char **error)
{
    int argc = 0;
    char *p = line;

    while (1) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == '\0')
            return argc;
        if (argc == max) {
            *error = "too many arguments";
            return -1;
        }
        char *start = p;
        char *out = p;
        if (*p == '"') {
            p++;
            while (*p != '"') {
                if (*p == '\0') {
                    *error = "unterminated quote";
                    return -1;
                }
                if (*p == '\\' && p[1] == 'x' && hex_digit(p[2]) >= 0 && hex_digit(p[3]) >= 0) {
                    *out++ = (char)(hex_digit(p[2]) * 16 + hex_digit(p[3]));
                    p += 4;
                } else if (*p == '\\' && unescape(p[1]) >= 0) {
                    *out++ = (char)unescape(p[1]);
                    p += 2;
                } else {
                    *out++ = *p++;
                }
            }
            p++;
            if (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                *error = "a closing quote must be followed by a space";
                return -1;
            }
        } else {
            while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
                out++, p++;
        }
        argv[argc] = start;
        lens[argc] = out - start;
        argc++;
        if (*p != '\0')
            p++;
        *out = '\0';
    }
}

void print_quoted(FILE *out, const char *s, size_t len)
{
    int plain = len > 0;
    for (size_t i = 0; i < len && plain; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '"' || c == '\\' || c < 32 || c == 127)
            plain = 0;
    }
    if (plain) {
        fwrite(s, 1, len, out);
        return;
    }
    fputc('"', out);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\t': fputs("\\t", out); break;
        case '\r': fputs("\\r", out); break;
        default:
            if (c < 32 || c == 127)
                fprintf(out, "\\x%02x", c);
            else
                fputc(c, out);
        }
    }
    fputc('"', out);
}
