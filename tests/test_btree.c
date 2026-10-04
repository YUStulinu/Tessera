/*
 * B+tree tests through the public API.
 *
 * The central one is a model test: thousands of random puts, overwrites and
 * deletes - with keys from a tiny alphabet (so they share prefixes and hit
 * the 0x00 / 0xFF edges) and values from empty to multi-page - applied both
 * to the database and to a plain sorted array. Periodically the whole
 * database is compared with the array and run through the integrity checker.
 */
#include <stdlib.h>
#include <string.h>

#include "engine/db_internal.h"
#include "test.h"
#include "util/common.h"

/* ------------------------------------------------------------------------
 * Reference model: a sorted array of (key, value)
 * ---------------------------------------------------------------------- */
typedef struct {
    uint8_t *k, *v;
    size_t kl, vl;
} entry;

typedef struct {
    entry *e;
    size_t n, cap;
} model;

static size_t model_find(const model *m, const uint8_t *k, size_t kl, int *found) {
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = ts_keycmp(m->e[mid].k, m->e[mid].kl, k, kl);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < m->n && ts_keycmp(m->e[lo].k, m->e[lo].kl, k, kl) == 0;
    return lo;
}

static uint8_t *dup_bytes(const uint8_t *p, size_t n) {
    uint8_t *d = (uint8_t *)malloc(n ? n : 1);
    if (n) memcpy(d, p, n);
    return d;
}

static void model_put(model *m, const uint8_t *k, size_t kl, const uint8_t *v, size_t vl) {
    int found = 0;
    size_t i = model_find(m, k, kl, &found);
    if (found) {
        free(m->e[i].v);
        m->e[i].v = dup_bytes(v, vl);
        m->e[i].vl = vl;
        return;
    }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 256;
        m->e = (entry *)realloc(m->e, m->cap * sizeof *m->e);
    }
    memmove(&m->e[i + 1], &m->e[i], (m->n - i) * sizeof *m->e);
    m->e[i].k = dup_bytes(k, kl);
    m->e[i].kl = kl;
    m->e[i].v = dup_bytes(v, vl);
    m->e[i].vl = vl;
    m->n++;
}

static int model_del(model *m, const uint8_t *k, size_t kl) {
    int found = 0;
    size_t i = model_find(m, k, kl, &found);
    if (!found) return 0;
    free(m->e[i].k);
    free(m->e[i].v);
    memmove(&m->e[i], &m->e[i + 1], (m->n - i - 1) * sizeof *m->e);
    m->n--;
    return 1;
}

static void model_free(model *m) {
    for (size_t i = 0; i < m->n; i++) {
        free(m->e[i].k);
        free(m->e[i].v);
    }
    free(m->e);
    memset(m, 0, sizeof *m);
}

/* Full comparison: iteration order, keys, values, count, and integrity. */
static int verify_db(ts_db *db, const model *m) {
    ts_iter *it = NULL;
    if (ts_iter_open(db, NULL, 0, NULL, 0, &it) != TS_OK) return 0;
    size_t i = 0;
    int ok = 1, rc;
    while (ok && (rc = ts_iter_next(it)) == TS_OK) {
        size_t kl = 0, vl = 0;
        int vrc = TS_OK;
        const uint8_t *k = (const uint8_t *)ts_iter_key(it, &kl);
        const uint8_t *v = (const uint8_t *)ts_iter_value(it, &vl, &vrc);
        if (i >= m->n || vrc != TS_OK || kl != m->e[i].kl || memcmp(k, m->e[i].k, kl) != 0 || vl != m->e[i].vl ||
            (vl && memcmp(v, m->e[i].v, vl) != 0))
            ok = 0;
        i++;
    }
    ts_iter_close(it);
    if (i != m->n) ok = 0;
    ts_stats st;
    ts_get_stats(db, &st);
    if (st.keys != m->n) ok = 0;
    uint64_t problems = 1;
    if (ts_check(db, NULL, NULL, &problems) != TS_OK || problems != 0) ok = 0;
    return ok;
}

static size_t random_key(uint64_t *rng, uint8_t *k) {
    static const uint8_t alphabet[] = {0x00, 'a', 'b', 'c', 0xFF};
    size_t len = ts_splitmix64(rng) % 24;
    if (ts_splitmix64(rng) % 50 == 0) len = 200 + ts_splitmix64(rng) % 800; /* occasionally long keys */
    for (size_t i = 0; i < len; i++) k[i] = alphabet[ts_splitmix64(rng) % sizeof alphabet];
    return len;
}

static size_t random_value(uint64_t *rng, uint8_t *v) {
    uint64_t c = ts_splitmix64(rng) % 100;
    size_t len = c < 70 ? ts_splitmix64(rng) % 200 : c < 95 ? 200 + ts_splitmix64(rng) % 800 : 1000 + ts_splitmix64(rng) % 12000;
    for (size_t i = 0; i < len; i++) v[i] = (uint8_t)ts_splitmix64(rng);
    return len;
}

static void model_test(uint64_t seed, int ops) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    model m = {0};
    uint64_t rng = seed;
    uint8_t *key = (uint8_t *)malloc(TS_MAX_KEY_SIZE);
    uint8_t *val = (uint8_t *)malloc(16000);

    for (int i = 0; i < ops; i++) {
        /* Phases alternate: autocommit, then transactions of 50 operations. */
        int in_batch = (i / 1000) % 2 == 1;
        if (in_batch && i % 50 == 0) CHECK_OK(ts_begin(db));

        size_t kl = random_key(&rng, key);
        if (ts_splitmix64(&rng) % 100 < 30) {
            int expect = model_del(&m, key, kl) ? TS_OK : TS_NOTFOUND;
            CHECK_RC(ts_delete(db, key, kl), expect);
        } else {
            size_t vl = random_value(&rng, val);
            CHECK_OK(ts_put(db, key, kl, val, vl));
            model_put(&m, key, kl, val, vl);
        }

        if (in_batch && i % 50 == 49) CHECK_OK(ts_commit(db));
        if (i % 2500 == 2499) {
            if (ts_in_transaction(db)) CHECK_OK(ts_commit(db));
            CHECK(verify_db(db, &m));
        }
        if (i % 7000 == 6999) {
            /* Close and reopen: everything must come back from the files. */
            if (ts_in_transaction(db)) CHECK_OK(ts_commit(db));
            CHECK_OK(ts_close(db));
            CHECK_OK(test_open_mem(vfs, &db));
        }
    }
    if (ts_in_transaction(db)) CHECK_OK(ts_commit(db));
    CHECK(verify_db(db, &m));

    CHECK_OK(ts_close(db));
    model_free(&m);
    free(key);
    free(val);
    ts_memvfs_destroy(vfs);
}

static void test_btree_model_small(void) {
    model_test(test_seed(), test_long() ? 200000 : 20000);
}

static void test_btree_model_more_seeds(void) {
    for (uint64_t s = 1; s <= (test_long() ? 20u : 4u); s++) model_test(test_seed() * 31 + s, 8000);
}

static void test_btree_basics_and_limits(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    void *v = NULL;
    size_t vl = 0;

    CHECK_RC(ts_get(db, "missing", 7, &v, &vl), TS_NOTFOUND);
    CHECK_OK(ts_put(db, "a", 1, "1", 1));
    CHECK_OK(ts_put(db, "a", 1, "22", 2)); /* overwrite */
    CHECK_OK(ts_get(db, "a", 1, &v, &vl));
    CHECK(vl == 2 && memcmp(v, "22", 2) == 0);
    ts_free(v);

    CHECK_OK(ts_put(db, "", 0, "empty key", 9)); /* the empty key is a valid key */
    CHECK_OK(ts_get(db, "", 0, &v, &vl));
    CHECK(vl == 9);
    ts_free(v);
    CHECK_OK(ts_put(db, "nothing", 7, "", 0)); /* and an empty value */
    CHECK_OK(ts_get(db, "nothing", 7, &v, &vl));
    CHECK(vl == 0);
    ts_free(v);

    uint8_t *big = (uint8_t *)malloc(3u << 20);
    for (size_t i = 0; i < (3u << 20); i++) big[i] = (uint8_t)(i * 31 + (i >> 11));
    char longkey[TS_MAX_KEY_SIZE + 1];
    memset(longkey, 'k', sizeof longkey);
    CHECK_OK(ts_put(db, longkey, TS_MAX_KEY_SIZE, big, 3u << 20));
    CHECK_RC(ts_put(db, longkey, TS_MAX_KEY_SIZE + 1, "x", 1), TS_INVALID);
    CHECK_RC(ts_put(db, "x", 1, big, (size_t)TS_MAX_VALUE_SIZE + 1), TS_TOOBIG);
    CHECK_OK(ts_get(db, longkey, TS_MAX_KEY_SIZE, &v, &vl));
    CHECK(vl == (3u << 20) && memcmp(v, big, vl) == 0);
    ts_free(v);

    /* Replacing a big value with a small one frees its overflow pages. */
    ts_stats before, after;
    ts_get_stats(db, &before);
    CHECK_OK(ts_put(db, longkey, TS_MAX_KEY_SIZE, "small", 5));
    ts_get_stats(db, &after);
    CHECK(after.free_pages >= before.free_pages + 700);

    /* Values right at the inline/overflow boundary. */
    for (size_t len = 990; len <= 1030; len++) {
        CHECK_OK(ts_put(db, "edge", 4, big, len));
        CHECK_OK(ts_get(db, "edge", 4, &v, &vl));
        CHECK(vl == len && memcmp(v, big, len) == 0);
        ts_free(v);
    }
    CHECK_RC(ts_delete(db, "a", 1), TS_OK);
    CHECK_RC(ts_delete(db, "a", 1), TS_NOTFOUND);

    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    free(big);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

static void test_btree_delete_all_reuses_pages(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    enum { N = 6000 };
    uint32_t order[N];
    char key[16];
    uint8_t val[300];
    memset(val, 'v', sizeof val);
    for (uint32_t i = 0; i < N; i++) order[i] = i;
    uint64_t rng = test_seed();

    CHECK_OK(ts_begin(db));
    for (uint32_t i = 0; i < N; i++) {
        snprintf(key, sizeof key, "%08u", (unsigned)order[i]);
        CHECK_OK(ts_put(db, key, 8, val, 50 + i % 250));
    }
    CHECK_OK(ts_commit(db));
    ts_stats full;
    ts_get_stats(db, &full);
    CHECK(full.tree_height >= 2);

    for (uint32_t i = N; i > 1; i--) {
        uint32_t j = (uint32_t)(ts_splitmix64(&rng) % i), t = order[i - 1];
        order[i - 1] = order[j];
        order[j] = t;
    }
    CHECK_OK(ts_begin(db));
    for (uint32_t i = 0; i < N; i++) {
        snprintf(key, sizeof key, "%08u", (unsigned)order[i]);
        CHECK_OK(ts_delete(db, key, 8));
        if (i % 1000 == 0) {
            uint64_t problems = 1;
            CHECK_OK(ts_check(db, NULL, NULL, &problems));
            CHECK(problems == 0);
        }
    }
    CHECK_OK(ts_commit(db));

    ts_stats empty;
    ts_get_stats(db, &empty);
    CHECK(empty.keys == 0);
    CHECK(empty.tree_height == 1);
    CHECK(empty.free_pages == empty.pages - 2); /* everything but the header and the root leaf */

    /* Inserting the same data again must reuse the freed pages, not grow the file. */
    CHECK_OK(ts_begin(db));
    for (uint32_t i = 0; i < N; i++) {
        snprintf(key, sizeof key, "%08u", (unsigned)i);
        CHECK_OK(ts_put(db, key, 8, val, 50 + i % 250));
    }
    CHECK_OK(ts_commit(db));
    ts_stats again;
    ts_get_stats(db, &again);
    CHECK(again.pages <= full.pages + 2);
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

static void test_btree_tall_tree(void) {
    /* Long keys that differ only at the end keep separators long, so internal
       nodes hold few of them and the tree grows tall quickly. */
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    char key[600];
    memset(key, 'p', 590);
    enum { N = 12000 };
    CHECK_OK(ts_begin(db));
    for (int i = 0; i < N; i++) {
        snprintf(key + 590, 10, "%08d", (i * 7919) % N);
        CHECK_OK(ts_put(db, key, 598, "v", 1));
    }
    CHECK_OK(ts_commit(db));
    ts_stats st;
    ts_get_stats(db, &st);
    CHECK(st.tree_height >= 4);
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);

    CHECK_OK(ts_begin(db));
    for (int i = 0; i < N; i++) {
        if (i % 10 == 0) continue;
        snprintf(key + 590, 10, "%08d", i);
        CHECK_OK(ts_delete(db, key, 598));
    }
    CHECK_OK(ts_commit(db));
    ts_stats after;
    ts_get_stats(db, &after);
    CHECK(after.keys == N / 10);
    CHECK(after.tree_height < st.tree_height);
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

static void test_iterator_ranges_and_concurrent_writes(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    char key[16];
    for (int i = 0; i < 1000; i += 2) {
        snprintf(key, sizeof key, "k%04d", i);
        CHECK_OK(ts_put(db, key, 5, key, 5));
    }

    /* [k0100, k0200): the even keys 100..198 */
    ts_iter *it = NULL;
    CHECK_OK(ts_iter_open(db, "k0100", 5, "k0200", 5, &it));
    int n = 0;
    while (ts_iter_next(it) == TS_OK) n++;
    CHECK(n == 50);
    CHECK_RC(ts_iter_next(it), TS_DONE);
    ts_iter_close(it);

    /* A lower bound between keys starts at the next key. */
    CHECK_OK(ts_iter_open(db, "k0101", 5, NULL, 0, &it));
    CHECK_OK(ts_iter_next(it));
    size_t kl = 0;
    CHECK(memcmp(ts_iter_key(it, &kl), "k0102", 5) == 0);
    ts_iter_close(it);

    /* Writes during iteration: delete the next key, insert odd keys ahead and
       behind, force splits. The iterator must keep moving forward, never
       repeat a key, see the inserts ahead of it and none of the deleted keys. */
    CHECK_OK(ts_iter_open(db, NULL, 0, NULL, 0, &it));
    char prev[16] = "";
    int seen = 0;
    while (ts_iter_next(it) == TS_OK) {
        const char *k = (const char *)ts_iter_key(it, &kl);
        CHECK(kl == 5);
        CHECK(strncmp(prev, k, 5) < 0 || prev[0] == '\0');
        memcpy(prev, k, 5); /* keys are not NUL-terminated; prev is */
        int num = atoi(prev + 1);
        if (num % 2 == 0 && num + 2 < 1000) {
            snprintf(key, sizeof key, "k%04d", num + 2);
            CHECK_OK(ts_delete(db, key, 5)); /* never returned */
            snprintf(key, sizeof key, "k%04d", num + 1);
            CHECK_OK(ts_put(db, key, 5, "odd", 3)); /* returned next */
        }
        seen++;
    }
    ts_iter_close(it);
    /* Visited: 0, 1, 4, 5, 8, 9, ... - every multiple of 4 (250 keys), each
       followed by the odd key inserted right after it (250 more). */
    CHECK(seen == 500);
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

void register_btree_tests(void) {
    test_add("btree: basics, limits, overflow values", test_btree_basics_and_limits);
    test_add("btree: random ops match a reference model", test_btree_model_small);
    test_add("btree: model test with more seeds", test_btree_model_more_seeds);
    test_add("btree: deleting everything frees every page", test_btree_delete_all_reuses_pages);
    test_add("btree: tall trees grow and shrink", test_btree_tall_tree);
    test_add("btree: iterators, ranges, writes mid-scan", test_iterator_ranges_and_concurrent_writes);
}
