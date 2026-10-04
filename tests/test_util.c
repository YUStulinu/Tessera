/*
 * Tests for the building blocks: CRC32C, the page-number hash map, the
 * in-memory file system's crash simulation, and separator keys.
 */
#include <stdlib.h>
#include <string.h>

#include "btree/node.h"
#include "os/vfs.h"
#include "os/vfs_mem.h"
#include "test.h"
#include "util/common.h"
#include "util/u32map.h"

static void test_crc32c_vectors(void) {
    CHECK(ts_crc32c(0, "123456789", 9) == 0xE3069283u);
    CHECK(ts_crc32c(0, "", 0) == 0);
    uint8_t zeros[32] = {0};
    CHECK(ts_crc32c(0, zeros, sizeof zeros) == 0x8A9136AAu); /* RFC 3720, B.4 */

    /* Chaining equals one pass, at every split point (exercises the 8-byte loop and the tail). */
    uint8_t data[100];
    for (int i = 0; i < 100; i++) data[i] = (uint8_t)(i * 7 + 3);
    uint32_t whole = ts_crc32c(0, data, sizeof data);
    for (size_t k = 0; k <= sizeof data; k++) CHECK(ts_crc32c(ts_crc32c(0, data, k), data + k, sizeof data - k) == whole);
}

static void test_u32map_against_reference(void) {
    u32map m;
    CHECK_OK(u32map_init(&m, 4));
    enum { KEYS = 3000 };
    static uint64_t ref[KEYS];
    static uint8_t has[KEYS];
    memset(has, 0, sizeof has);
    uint64_t rng = test_seed();
    size_t count = 0;
    for (int i = 0; i < 60000; i++) {
        uint64_t r = ts_splitmix64(&rng);
        uint32_t k = (uint32_t)(r % KEYS);
        if ((r >> 40) % 3 == 0) {
            CHECK(u32map_del(&m, k) == has[k]);
            if (has[k]) count--;
            has[k] = 0;
        } else {
            CHECK_OK(u32map_put(&m, k, r));
            if (!has[k]) count++;
            has[k] = 1;
            ref[k] = r;
        }
    }
    CHECK(m.count == count);
    for (uint32_t k = 0; k < KEYS; k++) {
        uint64_t v = 0;
        int found = u32map_get(&m, k, &v);
        CHECK(found == has[k]);
        if (found) CHECK(v == ref[k]);
    }
    u32map_free(&m);
}

static void test_memvfs_crash_modes(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_file *f = NULL;
    CHECK_OK(vfs->open(vfs, "f", 1, &f));
    CHECK_OK(vfs_write(f, "durable", 7, 0));
    CHECK_OK(vfs_sync(f));
    CHECK_OK(vfs_write(f, "VOLATILE", 8, 7));
    vfs_close(f);

    ts_memvfs_crash(vfs, TS_CRASH_LOSE_UNSYNCED, 1);
    uint64_t size = 0;
    uint8_t *d = ts_memvfs_file_data(vfs, "f", &size);
    CHECK(size == 7 && memcmp(d, "durable", 7) == 0);

    CHECK_OK(vfs->open(vfs, "f", 1, &f));
    CHECK_OK(vfs_write(f, "KEPT", 4, 7));
    vfs_close(f);
    ts_memvfs_crash(vfs, TS_CRASH_KEEP_ALL, 1);
    d = ts_memvfs_file_data(vfs, "f", &size);
    CHECK(size == 11 && memcmp(d, "durableKEPT", 11) == 0);

    /* After a crash the surviving bytes are durable: a second crash keeps them. */
    ts_memvfs_crash(vfs, TS_CRASH_LOSE_UNSYNCED, 1);
    d = ts_memvfs_file_data(vfs, "f", &size);
    CHECK(size == 11);

    /* I/O budget: the device "disappears" after N mutating operations. */
    CHECK_OK(vfs->open(vfs, "f", 1, &f));
    ts_memvfs_set_io_budget(vfs, 1);
    CHECK_OK(vfs_write(f, "x", 1, 0));
    CHECK_RC(vfs_write(f, "y", 1, 1), TS_IOERR);
    CHECK_RC(vfs_sync(f), TS_IOERR);
    vfs_close(f);
    ts_memvfs_destroy(vfs);
}

static void test_separator_property(void) {
    uint64_t rng = test_seed() + 7;
    for (int i = 0; i < 20000; i++) {
        uint8_t a[8], b[8];
        size_t alen = ts_splitmix64(&rng) % 8, blen = ts_splitmix64(&rng) % 8;
        for (size_t k = 0; k < 8; k++) {
            uint8_t v = (uint8_t)(ts_splitmix64(&rng) % 3); /* tiny alphabet: many shared prefixes */
            a[k] = v;
            b[k] = (uint8_t)(k < 3 ? v : ts_splitmix64(&rng) % 3);
        }
        int c = ts_keycmp(a, alen, b, blen);
        if (c == 0) continue;
        const uint8_t *lo = c < 0 ? a : b, *hi = c < 0 ? b : a;
        size_t lolen = c < 0 ? alen : blen, hilen = c < 0 ? blen : alen;
        size_t s = make_separator(lo, lolen, hi, hilen);
        /* lo < sep <= hi, where sep is the first s bytes of hi */
        CHECK(s >= 1 && s <= hilen);
        CHECK(ts_keycmp(lo, lolen, hi, s) < 0);
        CHECK(ts_keycmp(hi, s, hi, hilen) <= 0);
        /* and it is the shortest such prefix */
        if (s > 1) CHECK(ts_keycmp(lo, lolen, hi, s - 1) >= 0);
    }
}

void register_util_tests(void) {
    test_add("util: crc32c known vectors and chaining", test_crc32c_vectors);
    test_add("util: u32map against a reference array", test_u32map_against_reference);
    test_add("util: memvfs crash modes and I/O budget", test_memvfs_crash_modes);
    test_add("util: separators are shortest and in bounds", test_separator_property);
}
