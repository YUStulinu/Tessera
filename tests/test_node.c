/*
 * Slotted-page tests: insert/remove/compact keep a page well-formed and
 * sorted, and free-space accounting is exact.
 */
#include <stdlib.h>
#include <string.h>

#include "btree/node.h"
#include "test.h"

static size_t leaf_cell(uint8_t *buf, const char *key, size_t vlen) {
    size_t klen = strlen(key);
    put_u16(buf, (uint16_t)klen);
    put_u32(buf + 2, (uint32_t)vlen);
    memcpy(buf + LEAF_CELL_HEADER, key, klen);
    memset(buf + LEAF_CELL_HEADER + klen, 'v', vlen);
    return LEAF_CELL_HEADER + klen + vlen;
}

static void test_node_insert_remove_compact(void) {
    uint8_t *pg = (uint8_t *)calloc(1, TS_PAGE_SIZE);
    node_init(pg, NODE_LEAF);
    CHECK(node_sane(pg));
    CHECK(node_free(pg) == NODE_SPACE);

    uint64_t rng = test_seed();
    char key[32];
    uint8_t cell[NODE_MAX_CELL];
    int inserted = 0;
    for (int i = 0; i < 400; i++) {
        snprintf(key, sizeof key, "k%05u", (unsigned)(ts_splitmix64(&rng) % 100000));
        size_t sz = leaf_cell(cell, key, ts_splitmix64(&rng) % 40);
        bool exact = false;
        unsigned idx = node_lower_bound(pg, (const uint8_t *)key, strlen(key), &exact);
        if (exact) continue;
        size_t before = node_free(pg);
        if (!node_insert(pg, idx, cell, sz)) {
            CHECK(before < sz + 2); /* refused only when it really does not fit */
            break;
        }
        CHECK(node_free(pg) == before - sz - 2);
        inserted++;
        CHECK(node_sane(pg));

        /* Remove a random cell every few inserts, to create fragmentation. */
        if (i % 3 == 0 && node_count(pg) > 1) {
            unsigned victim = (unsigned)(ts_splitmix64(&rng) % node_count(pg));
            size_t vsz = cell_size(NODE_LEAF, node_cell(pg, victim));
            size_t f = node_free(pg);
            node_remove(pg, victim);
            CHECK(node_free(pg) == f + vsz + 2);
            CHECK(node_sane(pg));
        }
    }
    CHECK(inserted > 20);

    /* Keys are strictly increasing. */
    for (unsigned i = 1; i < node_count(pg); i++) {
        size_t al = 0, bl = 0;
        const uint8_t *a = cell_key(NODE_LEAF, node_cell(pg, i - 1), &al);
        const uint8_t *b = cell_key(NODE_LEAF, node_cell(pg, i), &bl);
        CHECK(ts_keycmp(a, al, b, bl) < 0);
    }

    size_t free_before = node_free(pg);
    node_compact(pg);
    CHECK(node_sane(pg));
    CHECK(node_free(pg) == free_before);
    CHECK(get_u16(pg + 6) == 0); /* no fragments left */
    free(pg);
}

static void test_cell_list_roundtrip(void) {
    uint8_t *pg = (uint8_t *)calloc(1, TS_PAGE_SIZE);
    uint8_t *copy = (uint8_t *)calloc(1, TS_PAGE_SIZE);
    node_init(pg, NODE_LEAF);
    uint8_t cell[NODE_MAX_CELL];
    char key[16];
    for (unsigned i = 0; i < 50; i++) {
        snprintf(key, sizeof key, "key%03u", i);
        size_t sz = leaf_cell(cell, key, i % 17);
        CHECK(node_insert(pg, i, cell, sz));
    }
    node_set_link(pg, 1234);

    cell_list l;
    cell_list_init(&l);
    CHECK_OK(cell_list_load(&l, pg));
    CHECK(l.n == 50);
    CHECK(cell_list_bytes(&l, 0, l.n) == node_used(pg));
    node_build(copy, NODE_LEAF, node_link(pg), &l, 0, l.n);
    CHECK(node_sane(copy));
    CHECK(node_count(copy) == 50 && node_link(copy) == 1234);
    for (unsigned i = 0; i < 50; i++) {
        const uint8_t *a = node_cell(pg, i), *b = node_cell(copy, i);
        CHECK(cell_size(NODE_LEAF, a) == cell_size(NODE_LEAF, b));
        CHECK(memcmp(a, b, cell_size(NODE_LEAF, a)) == 0);
    }
    cell_list_free(&l);
    free(pg);
    free(copy);
}

static void test_internal_child_index(void) {
    uint8_t *pg = (uint8_t *)calloc(1, TS_PAGE_SIZE);
    node_init(pg, NODE_INTERNAL);
    uint8_t cell[64];
    const char *seps[] = {"c", "f", "m"};
    for (unsigned i = 0; i < 3; i++) {
        size_t sz = make_internal_cell(cell, 10 + i, (const uint8_t *)seps[i], 1);
        CHECK(node_insert(pg, i, cell, sz));
    }
    node_set_link(pg, 99);
    /* child 0: < "c"; child 1: ["c","f"); child 2: ["f","m"); link: >= "m" */
    CHECK(node_child_at(pg, node_child_index(pg, (const uint8_t *)"a", 1)) == 10);
    CHECK(node_child_at(pg, node_child_index(pg, (const uint8_t *)"c", 1)) == 11);
    CHECK(node_child_at(pg, node_child_index(pg, (const uint8_t *)"e", 1)) == 11);
    CHECK(node_child_at(pg, node_child_index(pg, (const uint8_t *)"f", 1)) == 12);
    CHECK(node_child_at(pg, node_child_index(pg, (const uint8_t *)"m", 1)) == 99);
    CHECK(node_child_at(pg, node_child_index(pg, (const uint8_t *)"zz", 2)) == 99);
    CHECK(node_child_at(pg, node_child_index(pg, (const uint8_t *)"", 0)) == 10);
    free(pg);
}

void register_node_tests(void) {
    test_add("node: insert, remove, compact stay well-formed", test_node_insert_remove_compact);
    test_add("node: cell lists rebuild pages exactly", test_cell_list_roundtrip);
    test_add("node: internal nodes route keys to children", test_internal_child_index);
}
