/*
 * A deterministic random workload, and an in-memory model of what the
 * database must contain after it.
 *
 * Transaction t of a workload with seed s is a pure function of (s, t): the
 * crash test's parent can therefore recompute exactly what any committed
 * transaction did, without the child ever telling it more than a number.
 * The model stores, per key, only the seed and length of its value; the bytes
 * are regenerated when comparing, so the model stays small.
 *
 * Every transaction also overwrites a marker key with its own number, so a
 * recovered database says which workload transaction it ends with - even when
 * a transaction's other operations changed nothing (deleting absent keys),
 * which the engine does not count as a commit.
 */
#ifndef TS_WORKLOAD_H
#define TS_WORKLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "tessera.h"

#define WL_KEYSPACE 4000u
#define WL_MAX_OPS 16
#define WL_KEY_LEN 10
#define WL_MARKER "~txn" /* sorts after every workload key */
#define WL_MARKER_LEN 4

typedef struct {
    int      is_delete;
    uint32_t key_id;
    uint32_t vlen;
    uint64_t vseed;
} wl_op;

typedef struct {
    uint8_t  present;
    uint32_t vlen;
    uint64_t vseed;
} wl_slot;

/* The operations of transaction `txn`; returns how many (1..WL_MAX_OPS). */
int  wl_txn_ops(uint64_t seed, uint64_t txn, wl_op *ops);
void wl_key(uint32_t id, char out[WL_KEY_LEN + 1]);
void wl_value(uint64_t vseed, uint32_t vlen, uint8_t *out);

/* Applies transaction `txn` to the model / to the database (as one transaction). */
void wl_model_apply(wl_slot *model, uint64_t seed, uint64_t txn);
int  wl_db_apply(ts_db *db, uint64_t seed, uint64_t txn);

/* The workload transaction the database ends with (0 if none), from the marker. */
int  wl_db_txn(ts_db *db, uint64_t *txn);

/* Compares the whole database with the model. 0 if equal, else -1 with a message. */
int  wl_verify(ts_db *db, const wl_slot *model, char *err, size_t errlen);

#endif
