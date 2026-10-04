/*
 * Throughput benchmark.
 *
 *   tessera_bench [-n KEYS] [--sync full|normal|off] [--cache PAGES] [--dir DIR] [--value BYTES]
 *
 * Phases: sequential inserts and random updates/deletes in transactions of
 * 1000 operations, random point reads, a full scan, single-operation commits
 * (one fsync each, which is what durability costs), and a full integrity check.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "tessera.h"

#define BATCH 1000u

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void make_key(uint32_t i, char *out) {
    snprintf(out, 17, "user%012u", (unsigned)i);
}

static void shuffle(uint32_t *a, uint32_t n) {
    for (uint32_t i = n; i > 1; i--) {
        uint32_t j = (uint32_t)(rnd() % i);
        uint32_t t = a[i - 1];
        a[i - 1] = a[j];
        a[j] = t;
    }
}

static void report(const char *phase, uint64_t ops, double secs) {
    printf("  %-34s %10" PRIu64 " ops %9.3f s %12.0f ops/s\n", phase, ops, secs, secs > 0 ? (double)ops / secs : 0.0);
}

static int die(const char *what, int rc) {
    fprintf(stderr, "%s failed: %s\n", what, ts_errstr(rc));
    return 1;
}

int main(int argc, char **argv) {
    uint32_t n = 200000;
    size_t vsize = 100;
    const char *dir = ".";
    ts_sync_mode sync = TS_SYNC_FULL;
    const char *sync_name = "full";
    uint32_t cache = 0; /* 0: the library default */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) n = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--value") == 0 && i + 1 < argc) vsize = (size_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) cache = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (strcmp(argv[i], "--sync") == 0 && i + 1 < argc) {
            sync_name = argv[++i];
            sync = strcmp(sync_name, "off") == 0 ? TS_SYNC_OFF : strcmp(sync_name, "normal") == 0 ? TS_SYNC_NORMAL : TS_SYNC_FULL;
        } else {
            fprintf(stderr, "usage: %s [-n KEYS] [--value BYTES] [--sync full|normal|off] [--cache PAGES] [--dir DIR]\n", argv[0]);
            return 2;
        }
    }
    if (n < 2) n = 2;

    char path[1024], wal[1100];
    snprintf(path, sizeof path, "%s/tessera-bench.tdb", dir);
    snprintf(wal, sizeof wal, "%s-wal", path);
    remove(path);
    remove(wal);

    ts_options o;
    ts_options_default(&o);
    o.sync = sync;
    if (cache) o.cache_pages = cache;
    ts_db *db = NULL;
    int rc = ts_open(path, &o, &db);
    if (rc != TS_OK) return die("open", rc);

    uint32_t *order = (uint32_t *)malloc(n * sizeof *order);
    uint8_t *value = (uint8_t *)malloc(vsize ? vsize : 1);
    if (!order || !value) return 1;
    for (size_t i = 0; i < vsize; i++) value[i] = (uint8_t)('a' + i % 26);
    char key[17];

    printf("tessera benchmark: %u keys, %zu-byte values, sync=%s, cache %u pages\n", (unsigned)n, vsize,
           sync_name, (unsigned)o.cache_pages);

    /* 1. Sequential inserts. */
    double t0 = clock_now();
    for (uint32_t i = 0; i < n; i++) {
        if (i % BATCH == 0 && (rc = ts_begin(db)) != TS_OK) return die("begin", rc);
        make_key(i, key);
        if ((rc = ts_put(db, key, 16, value, vsize)) != TS_OK) return die("put", rc);
        if ((i + 1) % BATCH == 0 || i + 1 == n) {
            if ((rc = ts_commit(db)) != TS_OK) return die("commit", rc);
        }
    }
    report("insert, sequential keys", n, clock_now() - t0);

    /* 2. Random point reads. */
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    shuffle(order, n);
    t0 = clock_now();
    for (uint32_t i = 0; i < n; i++) {
        void *v = NULL;
        size_t vlen = 0;
        make_key(order[i], key);
        if ((rc = ts_get(db, key, 16, &v, &vlen)) != TS_OK) return die("get", rc);
        ts_free(v);
    }
    report("get, random keys", n, clock_now() - t0);

    /* 3. Full scan. */
    t0 = clock_now();
    ts_iter *it = NULL;
    if ((rc = ts_iter_open(db, NULL, 0, NULL, 0, &it)) != TS_OK) return die("iterator", rc);
    uint64_t scanned = 0;
    while (ts_iter_next(it) == TS_OK) {
        size_t vlen = 0;
        int vrc = TS_OK;
        (void)ts_iter_value(it, &vlen, &vrc);
        scanned++;
    }
    ts_iter_close(it);
    report("scan, all keys and values", scanned, clock_now() - t0);

    /* 4. Random updates, 5. random deletes. */
    shuffle(order, n);
    uint32_t half = n / 2;
    t0 = clock_now();
    for (uint32_t i = 0; i < half; i++) {
        if (i % BATCH == 0 && (rc = ts_begin(db)) != TS_OK) return die("begin", rc);
        make_key(order[i], key);
        value[0] = (uint8_t)rnd();
        if ((rc = ts_put(db, key, 16, value, vsize)) != TS_OK) return die("update", rc);
        if ((i + 1) % BATCH == 0 || i + 1 == half) {
            if ((rc = ts_commit(db)) != TS_OK) return die("commit", rc);
        }
    }
    report("update, random keys", half, clock_now() - t0);

    shuffle(order, n);
    t0 = clock_now();
    for (uint32_t i = 0; i < half; i++) {
        if (i % BATCH == 0 && (rc = ts_begin(db)) != TS_OK) return die("begin", rc);
        make_key(order[i], key);
        if ((rc = ts_delete(db, key, 16)) != TS_OK) return die("delete", rc);
        if ((i + 1) % BATCH == 0 || i + 1 == half) {
            if ((rc = ts_commit(db)) != TS_OK) return die("commit", rc);
        }
    }
    report("delete, random keys", half, clock_now() - t0);

    /* 6. One transaction per write: dominated by the fsync of every commit. */
    uint32_t singles = sync == TS_SYNC_FULL ? 500 : 5000;
    t0 = clock_now();
    for (uint32_t i = 0; i < singles; i++) {
        make_key(n + i, key);
        if ((rc = ts_put(db, key, 16, value, vsize)) != TS_OK) return die("put", rc);
    }
    report("put, one commit each", singles, clock_now() - t0);

    /* 7. Integrity check of the whole file. */
    t0 = clock_now();
    uint64_t problems = 0;
    if ((rc = ts_check(db, NULL, NULL, &problems)) != TS_OK) return die("check", rc);
    ts_stats st;
    ts_get_stats(db, &st);
    report("integrity check, pages", st.pages, clock_now() - t0);

    printf("  result: %" PRIu64 " keys, %u pages (%.1f MiB, %u free), tree height %u, %s\n", st.keys, st.pages,
           (double)st.pages * st.page_size / (1024.0 * 1024.0), st.free_pages, st.tree_height,
           problems ? "INTEGRITY PROBLEMS FOUND" : "integrity ok");
    printf("  cache: %" PRIu64 " hits, %" PRIu64 " misses; %" PRIu64 " checkpoints\n", st.cache_hits, st.cache_misses,
           st.checkpoints);

    rc = ts_close(db);
    remove(path);
    remove(wal);
    free(order);
    free(value);
    return (rc != TS_OK || problems) ? 1 : 0;
}
