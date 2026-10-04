/*
 * Transactions, persistence, locking, and error handling.
 */
#include <stdlib.h>
#include <string.h>

#include "engine/db_internal.h"
#include "os/vfs_mem.h"
#include "test.h"
#include "util/common.h"

static int count_keys(ts_db *db) {
    ts_stats st;
    ts_get_stats(db, &st);
    return (int)st.keys;
}

static void test_rollback_restores_everything(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    char key[16];
    for (int i = 0; i < 300; i++) {
        snprintf(key, sizeof key, "base%04d", i);
        CHECK_OK(ts_put(db, key, 8, "v", 1));
    }
    ts_stats before;
    ts_get_stats(db, &before);

    CHECK_OK(ts_begin(db));
    CHECK(ts_in_transaction(db));
    for (int i = 0; i < 3000; i++) { /* enough to split and allocate many pages */
        snprintf(key, sizeof key, "new%05d", i);
        CHECK_OK(ts_put(db, key, 8, "x", 1));
    }
    for (int i = 0; i < 300; i += 2) {
        snprintf(key, sizeof key, "base%04d", i);
        CHECK_OK(ts_delete(db, key, 8));
    }
    CHECK(count_keys(db) == 300 + 3000 - 150); /* the transaction sees its own writes */
    CHECK_OK(ts_rollback(db));
    CHECK(!ts_in_transaction(db));

    ts_stats after;
    ts_get_stats(db, &after);
    CHECK(after.keys == before.keys && after.pages == before.pages && after.txn_id == before.txn_id);
    void *v = NULL;
    size_t vl = 0;
    CHECK_RC(ts_get(db, "new00007", 8, &v, &vl), TS_NOTFOUND);
    CHECK_OK(ts_get(db, "base0000", 8, &v, &vl));
    ts_free(v);
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

static void test_transaction_misuse(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    CHECK_RC(ts_commit(db), TS_TXN);
    CHECK_RC(ts_rollback(db), TS_TXN);
    CHECK_OK(ts_begin(db));
    CHECK_RC(ts_begin(db), TS_TXN);
    CHECK_RC(ts_checkpoint(db), TS_TXN);
    CHECK_OK(ts_commit(db)); /* an empty transaction commits fine */
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

static void test_persistence_and_lock(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL, *second = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    CHECK_RC(test_open_mem(vfs, &second), TS_BUSY); /* one writer at a time */
    CHECK_OK(ts_put(db, "persist", 7, "me", 2));
    CHECK_OK(ts_close(db));

    CHECK_OK(test_open_mem(vfs, &db));
    void *v = NULL;
    size_t vl = 0;
    CHECK_OK(ts_get(db, "persist", 7, &v, &vl));
    CHECK(vl == 2 && memcmp(v, "me", 2) == 0);
    ts_free(v);
    CHECK_OK(ts_close(db));

    int exists = 1;
    CHECK_OK(vfs->exists(vfs, "test.tdb-wal", &exists));
    CHECK(!exists); /* a clean close checkpoints and removes the log */
    ts_memvfs_destroy(vfs);
}

static void test_not_a_database(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_file *f = NULL;
    CHECK_OK(vfs->open(vfs, "test.tdb", 1, &f));
    uint8_t junk[8192];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 13);
    CHECK_OK(vfs_write(f, junk, sizeof junk, 0));
    vfs_close(f);
    ts_db *db = NULL;
    CHECK_RC(test_open_mem(vfs, &db), TS_NOTADB);
    ts_memvfs_destroy(vfs);
}

static void test_corruption_is_detected_and_aborts(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    char key[16];
    CHECK_OK(ts_begin(db));
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "key%05d", i);
        CHECK_OK(ts_put(db, key, 8, "value", 5));
    }
    CHECK_OK(ts_commit(db));
    CHECK_OK(ts_close(db)); /* everything is in the database file now */

    /* Flip one byte in the middle of the file: some leaf now fails its checksum. */
    uint64_t size = 0;
    uint8_t *file = ts_memvfs_file_data(vfs, "test.tdb", &size);
    CHECK(file && size > 10 * TS_PAGE_SIZE);
    uint32_t victim = (uint32_t)(size / TS_PAGE_SIZE / 2);
    file[(uint64_t)victim * TS_PAGE_SIZE + 100] ^= 0x40;

    CHECK_OK(test_open_mem(vfs, &db));
    uint64_t problems = 0;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems > 0);

    /* Some key lives on the damaged page; find it through the API. */
    int corrupt_reads = 0;
    void *v = NULL;
    size_t vl = 0;
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "key%05d", i);
        int rc = ts_get(db, key, 8, &v, &vl);
        if (rc == TS_OK) ts_free(v);
        else if (rc == TS_CORRUPT) {
            corrupt_reads++;
            /* A write that hits the damage inside a transaction aborts it. */
            CHECK_OK(ts_begin(db));
            CHECK_OK(ts_put(db, "aaa-elsewhere", 13, "ok", 2));
            CHECK_RC(ts_put(db, key, 8, "new", 3), TS_CORRUPT);
            CHECK_RC(ts_put(db, "zzz", 3, "x", 1), TS_TXN);   /* aborted: refused */
            CHECK_RC(ts_commit(db), TS_TXN);                  /* reports it, ends it */
            CHECK_RC(ts_get(db, "aaa-elsewhere", 13, &v, &vl), TS_NOTFOUND); /* rolled back */
            break;
        } else {
            CHECK(0);
        }
    }
    CHECK(corrupt_reads == 1);
    ts_db_abandon(db);
    ts_memvfs_destroy(vfs);
}

static void test_failed_fsync_poisons_the_handle(void) {
    ts_vfs *vfs = ts_memvfs_create();
    ts_db *db = NULL;
    CHECK_OK(test_open_mem(vfs, &db));
    CHECK_OK(ts_put(db, "before", 6, "1", 1));
    ts_memvfs_fail_next_sync(vfs);
    CHECK_RC(ts_put(db, "during", 6, "2", 1), TS_IOERR);
    /* After a failed fsync the OS may have dropped the data: nothing is trusted any more. */
    CHECK_RC(ts_put(db, "after", 5, "3", 1), TS_POISONED);
    void *v = NULL;
    size_t vl = 0;
    CHECK_RC(ts_get(db, "before", 6, &v, &vl), TS_POISONED);
    CHECK_RC(ts_close(db), TS_POISONED);

    /* Reopening lets recovery decide from what is really on disk. */
    CHECK_OK(test_open_mem(vfs, &db));
    CHECK_OK(ts_get(db, "before", 6, &v, &vl));
    ts_free(v);
    int rc = ts_get(db, "during", 6, &v, &vl); /* either outcome is consistent */
    CHECK(rc == TS_OK || rc == TS_NOTFOUND);
    if (rc == TS_OK) ts_free(v);
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    CHECK_OK(ts_close(db));
    ts_memvfs_destroy(vfs);
}

static void test_real_files(void) {
    /* The same engine on the operating system's file system. */
    const char *dir = getenv("TMPDIR");
    if (!dir) dir = getenv("TEMP");
#ifdef _WIN32
    if (!dir) dir = ".";
#else
    if (!dir) dir = "/tmp";
#endif
    char path[1024], wal[1100];
    snprintf(path, sizeof path, "%s/tessera-unit-%llu.tdb", dir, (unsigned long long)test_seed());
    snprintf(wal, sizeof wal, "%s-wal", path);
    remove(path);
    remove(wal);

    ts_options o;
    ts_options_default(&o);
    ts_db *db = NULL, *second = NULL;
    CHECK_OK(ts_open(path, &o, &db));
    CHECK_RC(ts_open(path, &o, &second), TS_BUSY); /* flock / LockFileEx */
    char key[16];
    CHECK_OK(ts_begin(db));
    for (int i = 0; i < 5000; i++) {
        snprintf(key, sizeof key, "file%05d", i);
        CHECK_OK(ts_put(db, key, 9, key, 9));
    }
    CHECK_OK(ts_commit(db));
    ts_db_abandon(db); /* "crash": the commit is only in the log */

    CHECK_OK(ts_open(path, &o, &db));
    CHECK(count_keys(db) == 5000);
    uint64_t problems = 1;
    CHECK_OK(ts_check(db, NULL, NULL, &problems));
    CHECK(problems == 0);
    CHECK_OK(ts_close(db));
    CHECK_OK(ts_open(path, &o, &db));
    CHECK(count_keys(db) == 5000);
    CHECK_OK(ts_close(db));
    remove(path);
    remove(wal);
}

void register_txn_tests(void) {
    test_add("txn: rollback restores keys, pages, header", test_rollback_restores_everything);
    test_add("txn: misuse is reported", test_transaction_misuse);
    test_add("txn: data persists; second opener is BUSY", test_persistence_and_lock);
    test_add("txn: a non-database file is refused", test_not_a_database);
    test_add("txn: corruption detected; txn aborted", test_corruption_is_detected_and_aborts);
    test_add("txn: failed fsync poisons the handle", test_failed_fsync_poisons_the_handle);
    test_add("txn: real files, locks, log replay", test_real_files);
}
