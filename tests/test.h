/*
 * A minimal test framework: no dependencies, no macros magic beyond CHECK.
 * A failing CHECK records the failure and returns from the test function.
 */
#ifndef TS_TEST_H
#define TS_TEST_H

#include <stdint.h>
#include <stdio.h>

#include "tessera.h"

typedef void (*test_fn)(void);

void test_add(const char *name, test_fn fn);
void test_fail(const char *file, int line, const char *expr);
void test_fail_rc(const char *file, int line, const char *expr, int got, int want);
/* The seed for randomized tests (--seed N, or fixed by default). */
uint64_t test_seed(void);
/* Non-zero when run with --long: bigger randomized runs. */
int test_long(void);

#define CHECK(cond)                                     \
    do {                                                \
        if (!(cond)) {                                  \
            test_fail(__FILE__, __LINE__, #cond);       \
            return;                                     \
        }                                               \
    } while (0)

#define CHECK_RC(expr, want)                                         \
    do {                                                             \
        int rc_ = (expr);                                            \
        if (rc_ != (want)) {                                         \
            test_fail_rc(__FILE__, __LINE__, #expr, rc_, (want));    \
            return;                                                  \
        }                                                            \
    } while (0)

#define CHECK_OK(expr) CHECK_RC(expr, TS_OK)

/* Opens a database on an in-memory file system with small caches and
   frequent checkpoints, so tests exercise eviction and the log. */
int test_open_mem(ts_vfs *vfs, ts_db **db);

void register_util_tests(void);
void register_node_tests(void);
void register_btree_tests(void);
void register_txn_tests(void);
void register_recovery_tests(void);
void register_query_tests(void);

#endif
