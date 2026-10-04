/*
 * Query language: lexer, parser and executor, checked against exact output.
 */
#include <stdlib.h>
#include <string.h>

#include "query/exec.h"
#include "query/lexer.h"
#include "query/parser.h"
#include "test.h"

typedef struct {
    char  *buf;
    size_t len, cap;
} capture;

static void cap_write(void *ctx, const char *s, size_t n) {
    capture *c = (capture *)ctx;
    if (c->len + n + 1 > c->cap) {
        c->cap = (c->len + n + 1) * 2;
        c->buf = (char *)realloc(c->buf, c->cap);
    }
    memcpy(c->buf + c->len, s, n);
    c->len += n;
    c->buf[c->len] = '\0';
}

/* Runs a script on a fresh in-memory database and returns its output. */
static char *run(const char *script, int *failures) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    capture c = {0};
    cap_write(&c, "", 0);
    if (test_open_mem(vfs, &db) == TS_OK) {
        ts_writer w = {cap_write, &c};
        *failures = exec_script(db, script, strlen(script), &w, 0);
        ts_close(db);
    }
    ts_memvfs_destroy(vfs);
    return c.buf;
}

static void test_lexer_tokens_and_escapes(void) {
    const char *src = "PUT \"a b\\n\" x\\y; GET 'q\\x41\\x00' # comment\n-- another\nSCAN";
    lexer lx;
    lexer_init(&lx, src, strlen(src));
    token t;
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_WORD && t.len == 3 && memcmp(t.bytes, "PUT", 3) == 0);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_STRING && t.len == 4 && memcmp(t.bytes, "a b\n", 4) == 0);
    token_free(&t);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_WORD && t.len == 3 && memcmp(t.bytes, "x\\y", 3) == 0); /* a backslash is a plain byte in a word */
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_END);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_WORD);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_STRING && t.len == 3 && memcmp(t.bytes, "qA\0", 3) == 0);
    token_free(&t);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_END); /* the comment runs to the newline, which ends the statement */
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_END);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_WORD && t.line == 3 && t.col == 1);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_EOF);

    lexer_init(&lx, "\"open", 5);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_ERROR);
    lexer_init(&lx, "'\\x4'", 5);
    CHECK_OK(lexer_next(&lx, &t));
    CHECK(t.kind == TK_ERROR);
}

static void test_parser_statements_and_errors(void) {
    const char *src = "put k v\nscan from a to z limit 5 keys\ncount prefix user:\nincr n -3\nbegin;commit";
    parser p;
    parser_init(&p, src, strlen(src));
    stmt s;
    CHECK(parser_next(&p, &s) == 1);
    CHECK(s.kind == ST_PUT && s.key.len == 1 && s.value.len == 1);
    stmt_free(&s);
    CHECK(parser_next(&p, &s) == 1);
    CHECK(s.kind == ST_SCAN && s.has_from && s.has_to && s.has_limit && s.limit == 5 && s.keys_only);
    stmt_free(&s);
    CHECK(parser_next(&p, &s) == 1);
    CHECK(s.kind == ST_COUNT && s.has_prefix && s.prefix.len == 5);
    stmt_free(&s);
    CHECK(parser_next(&p, &s) == 1);
    CHECK(s.kind == ST_INCR && s.delta == -3);
    stmt_free(&s);
    CHECK(parser_next(&p, &s) == 1 && s.kind == ST_BEGIN);
    CHECK(parser_next(&p, &s) == 1 && s.kind == ST_COMMIT);
    CHECK(parser_next(&p, &s) == 0);
    parser_free(&p);

    const char *bad = "PUT a\nFROB x\nSCAN LIMIT many\nSCAN PREFIX a FROM b\nGET a b\nGET ok";
    parser_init(&p, bad, strlen(bad));
    CHECK(parser_next(&p, &s) == TS_INVALID);
    CHECK(strstr(p.error, "line 1, column 6") && strstr(p.error, "expected a value"));
    CHECK(parser_next(&p, &s) == TS_INVALID);
    CHECK(strstr(p.error, "unknown command"));
    CHECK(parser_next(&p, &s) == TS_INVALID);
    CHECK(strstr(p.error, "number after LIMIT"));
    CHECK(parser_next(&p, &s) == TS_INVALID); /* PREFIX and FROM are exclusive */
    CHECK(parser_next(&p, &s) == TS_INVALID);
    CHECK(strstr(p.error, "unexpected text"));
    CHECK(parser_next(&p, &s) == 1 && s.kind == ST_GET); /* parsing resumes after errors */
    stmt_free(&s);
    parser_free(&p);
}

static void test_exec_script_output(void) {
    int failures = -1;
    char *out = run("PUT a 1; PUT b \"two words\"; PUT c 3\n"
                    "GET a; GET b; GET nope\n"
                    "INCR counter; INCR counter 41; INCR b\n"
                    "SCAN FROM b\n"
                    "COUNT; COUNT PREFIX c\n"
                    "DEL a; DEL a; EXISTS a; EXISTS c",
                    &failures);
    const char *expected =
        "OK\nOK\nOK\n"
        "1\n\"two words\"\n(nil)\n"
        "1\n42\nerror: the stored value is not an integer\n"
        "b = \"two words\"\nc = 3\ncounter = 42\n(3 entries)\n"
        "4\n2\n"
        "OK\n(not found)\nfalse\ntrue\n";
    if (strcmp(out, expected) != 0) printf("\n--- got:\n%s--- expected:\n%s", out, expected);
    CHECK(strcmp(out, expected) == 0);
    CHECK(failures == 1); /* the INCR of a non-number */
    free(out);
}

static void test_exec_transactions_and_binary(void) {
    int failures = -1;
    char *out = run("BEGIN; PUT x 1; ROLLBACK; GET x\n"
                    "BEGIN; PUT \"\\x00\\xff\" \"tab\\there\"; COMMIT\n"
                    "SCAN\n"
                    "PUT ro \"Experiență, ș și ț\"; GET ro\n"
                    "COMMIT",
                    &failures);
    const char *expected =
        "OK\nOK\nOK\n(nil)\n"
        "OK\nOK\nOK\n"
        "\"\\x00\\xff\" = \"tab\\there\"\n(1 entry)\n"
        "OK\n\"Experiență, ș și ț\"\n"
        "error: no transaction to commit, or it was aborted by an error and rolled back\n";
    if (strcmp(out, expected) != 0) printf("\n--- got:\n%s--- expected:\n%s", out, expected);
    CHECK(strcmp(out, expected) == 0);
    CHECK(failures == 1);
    free(out);
}

static void test_exec_scan_limits_and_check(void) {
    int failures = -1;
    char *out = run("PUT k1 a; PUT k2 b; PUT k3 c; PUT z 9\n"
                    "SCAN PREFIX k LIMIT 2 KEYS\n"
                    "SCAN TO k2\n"
                    "CHECK",
                    &failures);
    CHECK(strstr(out, "k1\nk2\n(2 shown; more follow, use LIMIT to see more)\n") != NULL);
    CHECK(strstr(out, "k1 = a\n(1 entry)\n") != NULL);
    CHECK(strstr(out, "ok: ") != NULL && strstr(out, "no problems found") != NULL);
    CHECK(failures == 0);
    free(out);
}

static void test_literal_roundtrip(void) {
    /* Whatever the executor prints must parse back to the same bytes. */
    uint64_t rng = test_seed();
    for (int i = 0; i < 2000; i++) {
        unsigned char bytes[12];
        size_t n = (size_t)(rng % 12);
        for (size_t k = 0; k < n; k++) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            bytes[k] = (unsigned char)(rng >> 56);
        }
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        capture c = {0};
        ts_writer w = {cap_write, &c};
        exec_write_literal(&w, bytes, n);
        lexer lx;
        lexer_init(&lx, c.buf, c.len);
        token t;
        CHECK_OK(lexer_next(&lx, &t));
        CHECK(t.kind == TK_WORD || t.kind == TK_STRING);
        CHECK(t.len == n && (n == 0 || memcmp(t.bytes, bytes, n) == 0));
        token_free(&t);
        CHECK_OK(lexer_next(&lx, &t));
        CHECK(t.kind == TK_EOF);
        free(c.buf);
    }
}

void register_query_tests(void) {
    test_add("query: lexer tokens, escapes, comments", test_lexer_tokens_and_escapes);
    test_add("query: parser statements and error positions", test_parser_statements_and_errors);
    test_add("query: script output", test_exec_script_output);
    test_add("query: transactions, binary and UTF-8 values", test_exec_transactions_and_binary);
    test_add("query: SCAN limits, ranges, CHECK", test_exec_scan_limits_and_check);
    test_add("query: printed literals parse back unchanged", test_literal_roundtrip);
}
