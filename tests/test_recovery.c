/*
 * Power-loss tests on the in-memory file system.
 *
 * Each round gives the "disk" a random budget of I/O operations, runs a
 * deterministic workload until the disk dies (mid-commit, mid-checkpoint,
 * even mid-creation), then simulates a power cut - unsynced writes are lost,
 * or survive in a random subset with some torn at 512-byte sectors - and
 * reopens the database. With TS_SYNC_FULL the result must be exactly the
 * state after the last transaction whose commit returned success (or the one
 * after it, if its commit reached the disk before the failure was reported);
 * with TS_SYNC_NORMAL it may be older, but must still be exactly the state
 * after some committed transaction. Always: the integrity check passes.
 */
#include <stdlib.h>
#include <string.h>

#include "engine/db_internal.h"
#include "os/vfs_mem.h"
#include "test.h"
#include "util/common.h"
#include "workload.h"

static int open_with(ts_vfs *vfs, ts_sync_mode sync, ts_db **db) {
    ts_options o;
    ts_options_default(&o);
    o.vfs = vfs;
    o.sync = sync;
    o.cache_pages = 24;
    o.checkpoint_pages = 40; /* checkpoints happen often, so failures land inside them */
    return ts_open("power.tdb", &o, db);
}

typedef struct {
    int rounds, failed_in_open, in_flight_survived, rolled_back_further;
} power_stats;

/* One power-loss round. Returns through CHECK on any violation. */
static void power_round(uint64_t seed, int64_t budget, ts_crash_mode mode, ts_sync_mode sync, power_stats *ps) {
    ts_vfs *vfs = ts_memvfs_create();
    wl_slot *model = (wl_slot *)calloc(WL_KEYSPACE, sizeof *model);
    ts_db *db = NULL;
    uint64_t ok_txn = 0;

    ts_memvfs_set_io_budget(vfs, budget);
    int rc = open_with(vfs, sync, &db);
    if (rc == TS_OK) {
        for (uint64_t t = 1; t <= 300; t++) {
            if (wl_db_apply(db, seed, t) != TS_OK) break;
            ok_txn = t;
        }
        ts_db_abandon(db); /* the process "dies" with the disk */
    } else {
        ps->failed_in_open++;
    }
    ts_memvfs_crash(vfs, mode, seed ^ (uint64_t)budget);

    CHECK_OK(open_with(vfs, sync, &db));
    uint64_t d = 0;
    CHECK_OK(wl_db_txn(db, &d));
    if (d > ok_txn + 1 || (sync == TS_SYNC_FULL && d < ok_txn))
        printf("\n    seed %llu budget %lld mode %d: recovered transaction %llu, last acknowledged %llu",
               (unsigned long long)seed, (long long)budget, (int)mode, (unsigned long long)d,
               (unsigned long long)ok_txn);
    if (sync == TS_SYNC_FULL) {
        CHECK(d == ok_txn || d == ok_txn + 1);
    } else {
        CHECK(d <= ok_txn + 1);
        if (d < ok_txn) ps->rolled_back_further++;
    }
    if (d == ok_txn + 1) ps->in_flight_survived++;

    for (uint64_t t = 1; t <= d; t++) wl_model_apply(model, seed, t);
    char err[256] = "";
    if (wl_verify(db, model, err, sizeof err) != 0) {
        printf("\n    seed %llu budget %lld mode %d sync %d: transaction %llu (acknowledged %llu): %s",
               (unsigned long long)seed, (long long)budget, (int)mode, (int)sync, (unsigned long long)d,
               (unsigned long long)ok_txn, err);
        CHECK(0);
    }
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);

    /* The recovered database must be fully usable. */
    for (uint64_t t = d + 1; t <= d + 20; t++) {
        CHECK_OK(wl_db_apply(db, seed, t));
        wl_model_apply(model, seed, t);
    }
    CHECK(wl_verify(db, model, err, sizeof err) == 0);
    CHECK_OK(ts_close(db));
    ps->rounds++;
    free(model);
    ts_memvfs_destroy(vfs);
}

static void run_power_rounds(ts_sync_mode sync, int rounds) {
    power_stats ps = {0};
    uint64_t rng = test_seed() ^ (uint64_t)sync * 977;
    for (int r = 0; r < rounds; r++) {
        int64_t budget = (int64_t)(ts_splitmix64(&rng) % 700);
        ts_crash_mode mode = (ts_crash_mode)(ts_splitmix64(&rng) % 3);
        int before = ps.rounds;
        power_round(ts_splitmix64(&rng), budget, mode, sync, &ps);
        if (ps.rounds == before) return; /* the round failed a CHECK */
    }
    printf("[%d rounds: %d died during creation, %d in-flight commits kept%s] ", ps.rounds, ps.failed_in_open,
           ps.in_flight_survived, sync == TS_SYNC_NORMAL ? ", some lost unsynced commits" : "");
}

static void test_power_loss_sync_full(void) {
    run_power_rounds(TS_SYNC_FULL, test_long() ? 3000 : 250);
}

static void test_power_loss_sync_normal(void) {
    run_power_rounds(TS_SYNC_NORMAL, test_long() ? 1000 : 100);
}

static void test_uncommitted_work_disappears(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(open_with(vfs, TS_SYNC_FULL, &db));
    CHECK_OK(ts_put(db, "committed", 9, "yes", 3));
    CHECK_OK(ts_begin(db));
    CHECK_OK(ts_put(db, "uncommitted", 11, "no", 2));
    ts_db_abandon(db);
    ts_memvfs_crash(vfs, TS_CRASH_KEEP_ALL, 1);

    CHECK_OK(open_with(vfs, TS_SYNC_FULL, &db));
    void *v = NULL;
    size_t vl = 0;
    CHECK_OK(ts_get(db, "committed", 9, &v, &vl));
    ts_free(v);
    CHECK_RC(ts_get(db, "uncommitted", 11, &v, &vl), TS_NOTFOUND);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

static void test_log_replay_and_torn_tail(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(open_with(vfs, TS_SYNC_FULL, &db));
    char key[16];
    for (int i = 0; i < 30; i++) { /* fewer pages than the checkpoint threshold: all in the log */
        snprintf(key, sizeof key, "k%02d", i);
        CHECK_OK(ts_put(db, key, 3, key, 3));
    }
    ts_stats st;
    ts_get_stats(db, &st);
    CHECK(st.wal_frames > 0);
    ts_db_abandon(db);

    /* Tear the last frame of the log: the last transaction must vanish, the rest stay. */
    uint64_t size = 0;
    uint8_t *wal = ts_memvfs_file_data(vfs, "power.tdb-wal", &size);
    CHECK(wal && size > 32);
    wal[size - 100] ^= 0xFF;

    CHECK_OK(open_with(vfs, TS_SYNC_FULL, &db));
    void *v = NULL;
    size_t vl = 0;
    CHECK_OK(ts_get(db, "k28", 3, &v, &vl));
    ts_free(v);
    CHECK_RC(ts_get(db, "k29", 3, &v, &vl), TS_NOTFOUND);
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

void register_recovery_tests(void) {
    test_add("recovery: uncommitted work disappears", test_uncommitted_work_disappears);
    test_add("recovery: log replay; a torn tail is dropped", test_log_replay_and_torn_tail);
    test_add("recovery: power loss anywhere, sync=full", test_power_loss_sync_full);
    test_add("recovery: power loss anywhere, sync=normal", test_power_loss_sync_normal);
}
