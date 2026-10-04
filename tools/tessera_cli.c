/*
 * tessera - command-line shell for a Tessera database.
 *
 *   tessera mydata.tdb                      interactive
 *   tessera mydata.tdb -c "PUT a 1; GET a"  run commands and exit
 *   tessera mydata.tdb -f script.tql        run a file
 *   echo "SCAN" | tessera mydata.tdb        read commands from a pipe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query/exec.h"
#include "tessera.h"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define stdin_is_tty() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define stdin_is_tty() isatty(STDIN_FILENO)
#endif

static void write_stdout(void *ctx, const char *s, size_t n) {
    (void)ctx;
    fwrite(s, 1, n, stdout);
}

/* Reads one line of any length; NULL at end of input. */
static char *read_line(FILE *f, size_t *len) {
    size_t cap = 256, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > cap) {
            cap *= 2;
            char *b = (char *)realloc(buf, cap);
            if (!b) {
                free(buf);
                return NULL;
            }
            buf = b;
        }
        buf[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0 && c == EOF) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = (char *)malloc(cap);
    while (buf) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) break;
        cap *= 2;
        char *b = (char *)realloc(buf, cap);
        if (!b) {
            free(buf);
            buf = NULL;
        } else {
            buf = b;
        }
    }
    fclose(f);
    if (buf) *len = n;
    return buf;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options] <database> [-c \"commands\" | -f file]\n"
            "options:\n"
            "  --sync=full|normal|off   durability of commits (default full)\n"
            "  --cache=PAGES            page cache size in 4 KiB pages (default 2048)\n"
            "Type HELP inside the shell for the query language.\n",
            prog);
}

int main(int argc, char **argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    ts_options opt;
    ts_options_default(&opt);
    const char *path = NULL, *command = NULL, *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--sync=", 7) == 0) {
            const char *m = argv[i] + 7;
            opt.sync = strcmp(m, "off") == 0 ? TS_SYNC_OFF : strcmp(m, "normal") == 0 ? TS_SYNC_NORMAL : TS_SYNC_FULL;
        } else if (strncmp(argv[i], "--cache=", 8) == 0) {
            opt.cache_pages = (size_t)strtoul(argv[i] + 8, NULL, 10);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            file = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (!path && argv[i][0] != '-') {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!path) {
        usage(argv[0]);
        return 2;
    }

    ts_db *db = NULL;
    int rc = ts_open(path, &opt, &db);
    if (rc != TS_OK) {
        fprintf(stderr, "cannot open %s: %s\n", path, ts_errstr(rc));
        return 1;
    }
    ts_writer w = {write_stdout, NULL};
    int failures = 0;

    if (command) {
        failures = exec_script(db, command, strlen(command), &w, 1);
    } else if (file) {
        size_t len = 0;
        char *src = read_file(file, &len);
        if (!src) {
            fprintf(stderr, "cannot read %s\n", file);
            ts_close(db);
            return 1;
        }
        failures = exec_script(db, src, len, &w, 1);
        free(src);
    } else {
        int interactive = stdin_is_tty();
        if (interactive) printf("Tessera %s - %s. Type HELP for commands, .quit to exit.\n", TESSERA_VERSION, path);
        for (;;) {
            if (interactive) {
                fputs(ts_in_transaction(db) ? "tessera*> " : "tessera> ", stdout);
                fflush(stdout);
            }
            size_t len = 0;
            char *line = read_line(stdin, &len);
            if (!line) break;
            const char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (strncmp(t, ".quit", 5) == 0 || strncmp(t, ".exit", 5) == 0) {
                free(line);
                break;
            }
            failures += exec_script(db, line, len, &w, 0);
            fflush(stdout);
            free(line);
        }
        if (interactive) failures = 0; /* errors were shown; they are not the exit status */
    }

    if (ts_in_transaction(db)) fprintf(stderr, "note: the open transaction was rolled back\n");
    rc = ts_close(db);
    if (rc != TS_OK) {
        fprintf(stderr, "close: %s\n", ts_errstr(rc));
        return 1;
    }
    return failures ? 1 : 0;
}
