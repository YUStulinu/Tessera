/*
 * Crash test: kill a writer at random moments and check that recovery always
 * produces a consistent database that kept every acknowledged commit.
 *
 * Each round:
 *   1. A child process opens the database and runs transactions from a
 *      deterministic workload, printing each transaction's number after its
 *      commit returns (= after the fsync).
 *   2. The parent reads a random number of acknowledgements, waits a random
 *      0-3 ms so the kill lands anywhere - mid-transaction, mid-commit,
 *      mid-checkpoint - and kills the child with SIGKILL / TerminateProcess.
 *   3. The parent opens the database (which runs recovery) and checks:
 *        - durability: the last committed transaction D >= last acknowledged A
 *        - no phantoms: D <= A + 1 (the one in flight may or may not have made it)
 *        - atomicity:  the contents equal exactly the model after transaction D
 *        - integrity:  ts_check finds no problem at all
 *
 * A killed process loses nothing the OS already accepted, so this exercises
 * the engine's crash logic but not lost disk caches. Power loss - unsynced
 * writes dropped or torn - is simulated deterministically by the recovery
 * unit tests on the in-memory file system (tests/test_recovery.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine/db_internal.h"
#include "proc.h"
#include "tessera.h"
#include "workload.h"

static ts_sync_mode parse_sync(const char *s) {
    if (strcmp(s, "normal") == 0) return TS_SYNC_NORMAL;
    if (strcmp(s, "off") == 0) return TS_SYNC_OFF;
    return TS_SYNC_FULL;
}

static void child_options(ts_options *o, const char *sync) {
    ts_options_default(o);
    o->sync = parse_sync(sync);
    o->cache_pages = 64;       /* small: pages get evicted and re-read constantly */
    o->checkpoint_pages = 48;  /* frequent checkpoints, so kills often land inside one */
}

/* argv: --child <db> <seed> <first txn> <sync> */
static int child_main(int argc, char **argv) {
    if (argc < 5) return 2;
    const char *path = argv[1];
    uint64_t seed = strtoull(argv[2], NULL, 10);
    uint64_t start = strtoull(argv[3], NULL, 10);
    ts_options o;
    child_options(&o, argv[4]);

    ts_db *db = NULL;
    int rc = ts_open(path, &o, &db);
    if (rc != TS_OK) {
        printf("ERR open: %s\n", ts_errstr(rc));
        fflush(stdout);
        return 3;
    }
    ts_stats st;
    ts_get_stats(db, &st);
    if (st.txn_id != start - 1) {
        printf("ERR database is at transaction %llu, expected %llu\n", (unsigned long long)st.txn_id,
               (unsigned long long)(start - 1));
        fflush(stdout);
        return 4;
    }
    for (uint64_t t = start;; t++) {
        rc = wl_db_apply(db, seed, t);
        if (rc != TS_OK) {
            printf("ERR transaction %llu: %s\n", (unsigned long long)t, ts_errstr(rc));
            fflush(stdout);
            return 5;
        }
        printf("%llu\n", (unsigned long long)t);
        fflush(stdout);
    }
}

static void remove_db(const char *path) {
    char wal[1024];
    snprintf(wal, sizeof wal, "%s-wal", path);
    remove(path);
    remove(wal);
}

static void print_problem(void *ctx, const char *msg) {
    (void)ctx;
    fprintf(stderr, "    integrity: %s\n", msg);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--child") == 0) return child_main(argc - 1, argv + 1);

    int rounds = 50;
    const char *dir = ".";
    const char *sync = "full";
    uint64_t seed = (uint64_t)time(NULL);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--sync") == 0 && i + 1 < argc) sync = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--rounds N] [--dir DIR] [--seed N] [--sync full|normal]\n", argv[0]);
            return 2;
        }
    }

    char path[1024];
    snprintf(path, sizeof path, "%s/tessera-crashtest.tdb", dir);
    remove_db(path);
    printf("crash test: %d rounds, seed %llu, sync=%s, database %s\n", rounds, (unsigned long long)seed, sync, path);

    wl_slot *model = (wl_slot *)calloc(WL_KEYSPACE, sizeof *model);
    if (!model) return 1;
    uint64_t rng = seed ^ 0xC0FFEEull;
    uint64_t committed = 0;   /* last transaction known to be in the database */
    int in_flight_kept = 0, failures = 0;

    for (int r = 1; r <= rounds && !failures; r++) {
        char flag_s[] = "--child", seed_s[32], start_s[32], sync_s[16];
        snprintf(seed_s, sizeof seed_s, "%llu", (unsigned long long)seed);
        snprintf(start_s, sizeof start_s, "%llu", (unsigned long long)(committed + 1));
        snprintf(sync_s, sizeof sync_s, "%s", sync);
        char *child_argv[] = {flag_s, path, seed_s, start_s, sync_s};

        proc *p = NULL;
        if (proc_spawn_self(&p, 5, child_argv, child_main) != 0) {
            fprintf(stderr, "cannot start the writer process\n");
            return 1;
        }

        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t want = 1 + (rng >> 33) % 30;
        uint64_t acked = committed;
        char line[256];
        while (acked < committed + want) {
            int n = proc_read_line(p, line, sizeof line);
            if (n == 0) break;
            if (strncmp(line, "ERR", 3) == 0) {
                fprintf(stderr, "round %d: writer failed: %s\n", r, line);
                failures++;
                break;
            }
            acked = strtoull(line, NULL, 10);
        }
        sleep_ms((unsigned)((rng >> 40) % 4));
        proc_kill(p);
        /* Acknowledgements written before the kill may still be in the pipe. */
        while (proc_read_line(p, line, sizeof line) > 0) {
            if (strncmp(line, "ERR", 3) != 0) acked = strtoull(line, NULL, 10);
        }
        proc_free(p);
        if (failures) break;

        ts_options o;
        ts_options_default(&o);
        o.sync = parse_sync(sync);
        ts_db *db = NULL;
        int rc = ts_open(path, &o, &db);
        if (rc != TS_OK) {
            fprintf(stderr, "round %d: reopening after the crash failed: %s\n", r, ts_errstr(rc));
            failures++;
            break;
        }
        uint64_t d = 0;
        const char *verdict = NULL;
        if (wl_db_txn(db, &d) != TS_OK) verdict = "the transaction marker cannot be read";
        else if (d < acked) verdict = "an acknowledged commit was lost";
        else if (d > acked + 1) verdict = "the database has transactions that were never started";
        if (verdict) {
            fprintf(stderr, "round %d: FAILED: %s (acknowledged %llu, recovered %llu)\n", r, verdict,
                    (unsigned long long)acked, (unsigned long long)d);
            failures++;
            ts_db_abandon(db);
            break;
        }
        if (d == acked + 1) in_flight_kept++;
        for (uint64_t t = committed + 1; t <= d; t++) wl_model_apply(model, seed, t);
        committed = d;

        char err[256];
        if (wl_verify(db, model, err, sizeof err) != 0) {
            fprintf(stderr, "round %d: FAILED: contents differ from transaction %llu: %s\n", r,
                    (unsigned long long)d, err);
            failures++;
        }
        uint64_t problems = 0;
        rc = ts_check(db, print_problem, NULL, &problems);
        if (rc != TS_OK || problems) {
            fprintf(stderr, "round %d: FAILED: integrity check found %llu problem(s)\n", r,
                    (unsigned long long)problems);
            failures++;
        }
        ts_stats st;
        ts_get_stats(db, &st);
        printf("round %3d: killed after #%llu%s -> recovered #%llu, %llu keys, %u pages, %u WAL pages: ok\n", r,
               (unsigned long long)acked, d == acked + 1 ? " (+1 in flight)" : "", (unsigned long long)d,
               (unsigned long long)st.keys, st.pages, st.wal_frames);
        fflush(stdout);

        /* Alternate between a clean close (checkpoint) and abandoning the
           handle, so the next round sometimes starts with a non-empty log. */
        if (r % 2) ts_close(db);
        else ts_db_abandon(db);
    }

    free(model);
    if (failures) {
        printf("crash test FAILED (seed %llu)\n", (unsigned long long)seed);
        return 1;
    }
    remove_db(path);
    printf("crash test passed: %d kills, %llu transactions, %d in-flight commits survived the kill\n", rounds,
           (unsigned long long)committed, in_flight_kept);
    return 0;
}
