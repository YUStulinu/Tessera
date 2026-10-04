#include "workload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t mix(uint64_t a, uint64_t b) {
    uint64_t s = a ^ (b * 0x9E3779B97F4A7C15ull);
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t next(uint64_t *state) {
    *state = mix(*state, 0x2545F4914F6CDD1Dull);
    return *state;
}

int wl_txn_ops(uint64_t seed, uint64_t txn, wl_op *ops) {
    uint64_t st = mix(seed, txn);
    int n = 1 + (int)(next(&st) % WL_MAX_OPS);
    for (int i = 0; i < n; i++) {
        uint64_t r = next(&st);
        ops[i].key_id = (uint32_t)(r % WL_KEYSPACE);
        ops[i].is_delete = (r >> 32) % 100 < 25;
        uint64_t size_class = next(&st) % 100;
        uint64_t len;
        if (size_class < 60) len = next(&st) % 120;            /* small: inline */
        else if (size_class < 92) len = 120 + next(&st) % 900; /* medium: inline, fills pages fast */
        else len = 1500 + next(&st) % 22000;                   /* large: overflow chains */
        ops[i].vlen = (uint32_t)len;
        ops[i].vseed = mix(mix(seed, txn), (uint64_t)i + 1);
    }
    return n;
}

void wl_key(uint32_t id, char out[WL_KEY_LEN + 1]) {
    snprintf(out, WL_KEY_LEN + 1, "key-%06u", (unsigned)id);
}

void wl_value(uint64_t vseed, uint32_t vlen, uint8_t *out) {
    uint64_t st = vseed;
    uint32_t i = 0;
    while (i < vlen) {
        uint64_t r = next(&st);
        for (int b = 0; b < 8 && i < vlen; b++, i++) out[i] = (uint8_t)(r >> (8 * b));
    }
}

void wl_model_apply(wl_slot *model, uint64_t seed, uint64_t txn) {
    wl_op ops[WL_MAX_OPS];
    int n = wl_txn_ops(seed, txn, ops);
    for (int i = 0; i < n; i++) {
        wl_slot *s = &model[ops[i].key_id];
        if (ops[i].is_delete) {
            s->present = 0;
        } else {
            s->present = 1;
            s->vlen = ops[i].vlen;
            s->vseed = ops[i].vseed;
        }
    }
}

int wl_db_apply(ts_db *db, uint64_t seed, uint64_t txn) {
    wl_op ops[WL_MAX_OPS];
    int n = wl_txn_ops(seed, txn, ops);
    uint8_t *buf = (uint8_t *)malloc(24000);
    if (!buf) return TS_NOMEM;
    int rc = ts_begin(db);
    for (int i = 0; i < n && rc >= 0; i++) {
        char key[WL_KEY_LEN + 1];
        wl_key(ops[i].key_id, key);
        if (ops[i].is_delete) {
            rc = ts_delete(db, key, WL_KEY_LEN);
            if (rc == TS_NOTFOUND) rc = TS_OK;
        } else {
            wl_value(ops[i].vseed, ops[i].vlen, buf);
            rc = ts_put(db, key, WL_KEY_LEN, buf, ops[i].vlen);
        }
    }
    if (rc >= 0) {
        uint8_t mark[8];
        for (int b = 0; b < 8; b++) mark[b] = (uint8_t)(txn >> (8 * b));
        rc = ts_put(db, WL_MARKER, WL_MARKER_LEN, mark, sizeof mark);
    }
    free(buf);
    if (rc >= 0) return ts_commit(db);
    (void)ts_rollback(db);
    return rc;
}

int wl_db_txn(ts_db *db, uint64_t *txn) {
    void *v = NULL;
    size_t vlen = 0;
    *txn = 0;
    int rc = ts_get(db, WL_MARKER, WL_MARKER_LEN, &v, &vlen);
    if (rc == TS_NOTFOUND) return TS_OK;
    if (rc != TS_OK) return rc;
    if (vlen != 8) rc = TS_CORRUPT;
    for (size_t b = 0; b < vlen && b < 8; b++) *txn |= (uint64_t)((const uint8_t *)v)[b] << (8 * b);
    ts_free(v);
    return rc;
}

int wl_verify(ts_db *db, const wl_slot *model, char *err, size_t errlen) {
    ts_iter *it = NULL;
    int rc = ts_iter_open(db, NULL, 0, WL_MARKER, WL_MARKER_LEN, &it); /* workload keys only */
    if (rc != TS_OK) {
        snprintf(err, errlen, "cannot open iterator: %s", ts_errstr(rc));
        return -1;
    }
    uint8_t *expect = (uint8_t *)malloc(24000);
    uint32_t id = 0; /* keys are zero-padded, so key order == id order */
    int result = 0;
    while (result == 0 && (rc = ts_iter_next(it)) == TS_OK) {
        size_t klen = 0;
        const char *k = (const char *)ts_iter_key(it, &klen);
        while (id < WL_KEYSPACE && !model[id].present) id++;
        char want[WL_KEY_LEN + 1];
        if (id < WL_KEYSPACE) wl_key(id, want);
        if (id >= WL_KEYSPACE || klen != WL_KEY_LEN || memcmp(k, want, WL_KEY_LEN) != 0) {
            snprintf(err, errlen, "unexpected key %.*s (expected %s)", (int)klen, k,
                     id < WL_KEYSPACE ? want : "end of data");
            result = -1;
            break;
        }
        size_t vlen = 0;
        int vrc = TS_OK;
        const uint8_t *v = (const uint8_t *)ts_iter_value(it, &vlen, &vrc);
        wl_value(model[id].vseed, model[id].vlen, expect);
        if (vrc != TS_OK || vlen != model[id].vlen || (vlen && memcmp(v, expect, vlen) != 0)) {
            snprintf(err, errlen, "wrong value for %s (len %zu, expected %u)", want, vlen, (unsigned)model[id].vlen);
            result = -1;
        }
        id++;
    }
    if (result == 0 && rc != TS_DONE) {
        snprintf(err, errlen, "iteration failed: %s", ts_errstr(rc));
        result = -1;
    }
    if (result == 0) {
        while (id < WL_KEYSPACE && !model[id].present) id++;
        if (id < WL_KEYSPACE) {
            char want[WL_KEY_LEN + 1];
            wl_key(id, want);
            snprintf(err, errlen, "missing key %s", want);
            result = -1;
        }
    }
    free(expect);
    ts_iter_close(it);
    return result;
}
