/*
 * Test runner.
 *
 *   tessera_tests                 run everything
 *   tessera_tests btree           run tests whose name contains "btree"
 *   tessera_tests --seed 42       change the seed of the randomized tests
 *   tessera_tests --long          bigger randomized runs
 */
#include <stdlib.h>
#include <string.h>

#include "test.h"

#define MAX_TESTS 256

static struct {
    const char *name;
    test_fn fn;
} tests[MAX_TESTS];
static int ntests = 0;
static int current_failed = 0;
static uint64_t seed = 20261004;
static int long_mode = 0;

void test_add(const char *name, test_fn fn) {
    if (ntests < MAX_TESTS) {
        tests[ntests].name = name;
        tests[ntests].fn = fn;
        ntests++;
    }
}

void test_fail(const char *file, int line, const char *expr) {
    printf("\n    FAILED %s:%d: %s\n", file, line, expr);
    current_failed = 1;
}

void test_fail_rc(const char *file, int line, const char *expr, int got, int want) {
    printf("\n    FAILED %s:%d: %s returned %d (%s), expected %d (%s)\n", file, line, expr, got, ts_errstr(got), want,
           ts_errstr(want));
    current_failed = 1;
}

uint64_t test_seed(void) {
    return seed;
}

int test_long(void) {
    return long_mode;
}

int test_open_mem(ts_vfs *vfs, ts_db **db) {
    ts_options o;
    ts_options_default(&o);
    o.vfs = vfs;
    o.cache_pages = 32;
    o.checkpoint_pages = 64;
    return ts_open("test.tdb", &o, db);
}

int main(int argc, char **argv) {
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--long") == 0) long_mode = 1;
        else filter = argv[i];
    }

    register_util_tests();
    register_node_tests();
    register_btree_tests();
    register_txn_tests();
    register_recovery_tests();
    register_query_tests();

    int run = 0, failed = 0;
    for (int i = 0; i < ntests; i++) {
        if (filter && !strstr(tests[i].name, filter)) continue;
        printf("  %-48s", tests[i].name);
        fflush(stdout);
        current_failed = 0;
        tests[i].fn();
        run++;
        if (current_failed) failed++;
        else printf("ok\n");
        fflush(stdout);
    }
    printf("\n%d tests, %d failed (seed %llu)\n", run, failed, (unsigned long long)seed);
    return failed ? 1 : 0;
}
