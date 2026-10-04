# Tessera

A crash-safe key-value storage engine in C11, written from scratch with no
dependencies beyond the C standard library and the operating system:

- an **on-disk B+tree** with variable-length keys and values, overflow pages for
  large values, page merging on delete, and a free list that reuses pages;
- a **write-ahead log** with checksummed, chained frames and checkpoints;
- **crash recovery** that keeps every acknowledged commit and never exposes a
  partial one;
- **ACID transactions** (single writer), with rollback;
- a small **query language** with a REPL;
- an **integrity checker** that accounts for every page in the file;
- a **crash test** that kills the writing process at random moments and checks
  the recovered database, plus **simulated power loss** tests that drop and tear
  unsynced writes.

It builds warning-free with `-Wall -Wextra -Wpedantic -Werror` (GCC, Clang) and
`/W4 /WX` (MSVC), and runs on Linux, macOS and Windows.

```
$ tessera shop.tdb
tessera> PUT user:1 "Ana Pop"
OK
tessera> BEGIN
tessera*> PUT user:2 Ion
OK
tessera*> INCR visits
1
tessera*> COMMIT
OK
tessera> SCAN PREFIX user:
user:1 = "Ana Pop"
user:2 = Ion
(2 entries)
```

## Building

Requirements: CMake 3.20+ and a C11 compiler (GCC, Clang, or Visual Studio 2022).

```bash
cmake -S . -B build
cmake --build build --config Release
```

This produces four programs (under `build/` on Linux/macOS, `build/Release/` with Visual Studio):

| Program             | What it does                                          |
|---------------------|-------------------------------------------------------|
| `tessera`           | the command-line client (REPL and scripts)            |
| `tessera_tests`     | unit, model, recovery and query tests                 |
| `tessera_crashtest` | kills a writer process repeatedly and checks recovery |
| `tessera_bench`     | throughput benchmark                                  |

Optional: `-DTESSERA_SANITIZE=ON` builds with AddressSanitizer and
UndefinedBehaviorSanitizer (GCC/Clang); `-DTESSERA_WERROR=OFF` turns
warnings-as-errors off.

## Using the command-line client

```bash
tessera data.tdb                              # interactive
tessera data.tdb -c "PUT a 1; GET a"          # run commands
tessera data.tdb -f script.tql                # run a file
tessera --sync=normal --cache=8192 data.tdb   # options
```

The database file is created if it does not exist. In the REPL, the prompt
changes to `tessera*>` inside a transaction; `.quit` or end-of-input exits (an
open transaction is rolled back). With `-c` and `-f`, execution stops at the
first error and the exit code is 1.

### The query language

Commands are case-insensitive and are separated by `;` or new lines. `#` and
`--` start comments.

| Command                        | Effect                                                   |
|--------------------------------|----------------------------------------------------------|
| `PUT key value`                | insert or replace                                        |
| `GET key`                      | print the value, or `(nil)`                              |
| `DEL key` / `DELETE key`       | delete (`(not found)` if absent)                         |
| `EXISTS key`                   | `true` / `false`                                         |
| `INCR key [n]`                 | add `n` (default 1) to an integer value, atomically      |
| `SCAN [range] [LIMIT n] [KEYS]`| entries in key order; default limit 100; `KEYS` = no values |
| `COUNT [range]`                | number of keys                                           |
| `BEGIN` / `COMMIT` / `ROLLBACK`| explicit transaction; otherwise each write commits alone |
| `CHECK`                        | verify the whole database                                |
| `STATS`                        | sizes, tree height, cache and log counters               |
| `CHECKPOINT`                   | copy the log into the database file now                  |
| `HELP`                         | the list above                                           |

A range is `FROM a [TO b]`, `TO b` or `PREFIX p`; `TO` is exclusive.

Keys and values are arbitrary bytes. Bare words work for simple text; anything
else goes in quotes, with escapes `\" \\ \n \r \t \0 \xHH`:

```
PUT "full name" "Ana Pop"
PUT bin "\x00\xff\x10"
PUT ro "Experiență"           # UTF-8 is stored and printed as-is
SCAN FROM user: TO user;      # all keys starting with "user:"
COUNT PREFIX order:2026-
```

Output uses the same syntax, so anything the client prints can be pasted back.
Errors give a position: `syntax error, line 3, column 9: expected a key`.

If a command fails inside a transaction, the transaction is rolled back and
marked aborted: further writes are refused until `ROLLBACK` or `COMMIT`
(which then reports the abort). A transaction never commits half its work.

### Limits

Keys up to 1000 bytes, values up to 64 MiB, databases up to 16 TiB
(2^32 pages of 4 KiB). One process writes a database at a time; a second
`open` gets `TS_BUSY` (the file is locked with `flock` / `LockFileEx`).

## The C API

```c
#include "tessera.h"

ts_options opt;
ts_options_default(&opt);          /* sync=FULL, 2048-page cache (8 MiB) */
ts_db *db;
if (ts_open("data.tdb", &opt, &db) != TS_OK) { ... }

ts_put(db, "k", 1, "v", 1);        /* autocommit */

ts_begin(db);
ts_put(db, "a", 1, "1", 1);
ts_delete(db, "b", 1);             /* TS_NOTFOUND if absent */
ts_commit(db);                     /* durable when it returns (sync=FULL) */

void *v; size_t len;
if (ts_get(db, "a", 1, &v, &len) == TS_OK) { ...; ts_free(v); }

ts_iter *it;
ts_iter_open(db, "a", 1, "b", 1, &it);       /* [a, b) */
while (ts_iter_next(it) == TS_OK) {
    size_t kl, vl; int rc;
    const void *k = ts_iter_key(it, &kl);
    const void *val = ts_iter_value(it, &vl, &rc);
}
ts_iter_close(it);

ts_close(db);                      /* checkpoints and removes the log */
```

Every function returns `TS_OK` (0), a positive "soft" status (`TS_NOTFOUND`,
`TS_DONE`) or a negative error (`TS_IOERR`, `TS_CORRUPT`, `TS_TXN`, ...);
`ts_errstr()` describes it. The full API, with comments, is in
[include/tessera.h](include/tessera.h).

Sync modes:

| Mode             | Guarantee after a power cut                              | Cost                        |
|------------------|----------------------------------------------------------|-----------------------------|
| `TS_SYNC_FULL`   | every commit that returned is kept                       | one fsync per commit        |
| `TS_SYNC_NORMAL` | the database is consistent, recent commits may be lost   | fsync only at checkpoints   |
| `TS_SYNC_OFF`    | none after a power cut; a process crash is still safe    | no fsync                    |

## How it is built

```
  tessera.h (API)  ─  db.c: transactions, iterators, error policy
        │
  query/   lexer → parser → executor          (the language and the REPL)
        │
  btree/   B+tree: search, insert/split, delete/merge, cursors, overflow
        │
  storage/ pager: page cache, dirty pages, header, free list
        │        wal: log frames, commit, recovery, checkpoint
        │
  os/      VFS: POSIX · Win32 · in-memory with power-loss simulation
```

Each layer only talks to the one below it. The on-disk layout is documented
byte by byte in [docs/FORMAT.md](docs/FORMAT.md).

### Why a B+tree, and why this one

A B+tree keeps keys sorted on disk with a fan-out of hundreds per page, so a
lookup in a million keys reads 3 pages, and range scans walk the leaf chain
without going back up the tree. Details that matter:

- **Slotted pages.** Each node is a 4 KiB page with a sorted array of 2-byte
  slots at the front and variable-length cells packed from the back. Insertion
  shifts slots, not data; deleted space is reclaimed by compacting only when
  needed.
- **Splitting by bytes, not by count.** With keys of 1 byte to 1000 bytes and
  values of any size, half the cells is not half the page. Leaves split at the
  point that balances bytes, so both halves have room.
- **Overflow chains.** A cell may use at most a quarter of a page, which
  guarantees at least four cells per node (a split always has something to
  split). Larger values move to a chain of overflow pages, leaving a 4-byte
  pointer in the leaf.
- **Suffix-truncated separators.** Internal nodes store the shortest key that
  separates two leaves (`user:1` | `user:2` → separator `user:2`, but
  `alpha...` | `beta...` → just `b`), so internal nodes hold more children and
  the tree stays shallower.
- **Real deletes.** A node that drops below a quarter full merges with a
  sibling, or borrows cells from it if both together do not fit in one page.
  Merges cascade upwards and the root collapses when it has a single child, so
  deleting every key returns every page to the free list (a test checks this).
- **No parent pointers.** Splits and merges would have to rewrite every
  moved child to fix them. Instead each operation records its path from the
  root, which it already knows.
- **Stable cursors.** Writes during an iteration are allowed. Every write bumps
  a version number; a cursor that sees a changed version re-seeks to the first
  key after the one it returned last, so it never repeats or skips a live key.

### Why a write-ahead log

Updating pages in place is not atomic: a crash halfway through a split leaves
a tree with dangling pointers. Tessera never modifies the database file
during a transaction. A commit appends every changed page, as a whole page
image, to the log - in **one write followed by one fsync**. The transaction is
durable exactly when that fsync returns. Reads look in the log first, through
an in-memory index from page number to the newest frame.

Every so often (2000 frames by default, and at close) a **checkpoint** copies
the newest version of each page into the database file and empties the log.
The order is what makes it safe, and a crash at any point in it is covered by
the tests:

1. fsync the log, 2. write the pages into the database file, 3. fsync the
database file, 4. reset the log with new salts.

Each log frame carries the salts of its log generation and a checksum chained
from the previous frame. On open, recovery accepts frames only while both
match, and only up to the last frame marked as a commit. That cleanly
separates:

- a torn last write (the checksum fails), from
- a transaction that never committed (no commit marker), from
- stale frames of an earlier generation beyond the end of a shorter log (wrong salts).

Each page also carries its own CRC32C, so corruption in the database file is
detected the first time a page is read, instead of being returned as data.

**Physical logging** (whole pages) was chosen over logical logging (operations)
because recovery is then trivial and cannot itself fail halfway: replaying a
page image is idempotent. The cost is write amplification for small random
updates, visible in the benchmark below.

### Why the pager works the way it does

- **No-steal.** Dirty pages of an open transaction never reach the disk;
  rollback just drops them. No undo log is needed. (The cache grows past its
  limit for a very large transaction rather than writing uncommitted pages.)
- **LRU cache** of clean pages, with pinning so a page in use is never evicted.
- **Poisoning after a failed fsync.** After a failed fsync, Linux may already
  have dropped the dirty pages and cleared the error, so a retry can "succeed"
  without the data. (PostgreSQL lost data this way in 2018.) Tessera never
  retries. The handle refuses all further work with `TS_POISONED`, and the
  next open lets recovery decide from what is really on disk.
- **The file system is behind an interface (VFS).** The engine itself never
  calls the OS. Besides POSIX and Win32 there is an in-memory VFS that
  remembers which writes were fsynced. It can then simulate a power cut by
  dropping unsynced writes, keeping them, or keeping a random subset with some
  torn at 512-byte sector boundaries. It can also fail an fsync on purpose, or
  "die" after exactly N operations.

### Durability details that are easy to get wrong

- A newly created file is only durable once its **directory** is fsynced too.
- On macOS, `fsync` does not flush the drive cache; Tessera uses `F_FULLFSYNC`.
- On Linux, `fdatasync` is enough: it skips only timestamps, not the file size.
- A commit sorts its pages so that page 0 (the header) is the last frame. The
  commit marker then travels with the new header.
- Creating a database is itself transaction 0. A crash during creation
  leaves either nothing or a complete empty database.

## Testing

```bash
ctest --test-dir build -C Release --output-on-failure    # all tests
build/tessera_tests                     # unit tests (Windows: build/Release/...)
build/tessera_tests recovery --long     # 4000 power-loss rounds instead of 350
build/tessera_tests btree --seed 42     # a subset, another random seed
```

What is tested:

- **Model tests.** Tens of thousands of random puts and deletes, with keys and
  values of random sizes, compared after every step with a simple sorted-array
  model. The full integrity check runs periodically.
- **Structure.** Splits and merges at every tree level, deleting every key
  frees every page, limits (1000-byte keys, multi-megabyte values), iterators
  with writes in the middle of a scan.
- **Transactions.** Rollback restores keys, pages and header; misuse is
  reported; a second writer is refused; a non-database file is refused.
  Corruption is detected and aborts the transaction; a failed fsync poisons
  the handle.
- **Power loss** (`tests/test_recovery.c`). The in-memory disk dies after a
  random number of I/O operations: in a commit, in a checkpoint, in the log
  reset, even while the database is being created. Then it loses, keeps or
  tears the unsynced writes, and the database is reopened. With
  `sync=full`, the result must be exactly the state after the last
  acknowledged transaction (or the one in flight, if its commit reached the
  disk). With `sync=normal`, it must be exactly the state after *some*
  committed transaction. In both cases the integrity check must pass and the
  database must stay fully usable.
- **The query language.** Exact output of scripts, error positions, binary
  and UTF-8 values, and printed literals parsing back to the same bytes.

### The crash test: killing the process mid-write

```bash
build/tessera_crashtest --rounds 100                # sync=full
build/tessera_crashtest --rounds 100 --sync normal
build/tessera_crashtest --rounds 50 --seed 7 --dir /tmp
```

Each round starts a child process that opens the database and runs
transactions from a deterministic workload: up to 16 puts and deletes each,
values from empty to 23 KB, frequent checkpoints, a small cache. After each
commit returns, the child prints the transaction's number. The parent reads a
random number of these acknowledgements, waits 0-3 ms so the kill lands
anywhere, then kills the child with `SIGKILL` / `TerminateProcess`. It then
opens the database itself and checks:

- **durability**: the recovered transaction is at least the last acknowledged one;
- **no phantoms**: it is at most one more (the commit that was in flight);
- **atomicity**: the contents equal, key by key and byte by byte, a model that
  replays exactly the recovered transactions;
- **integrity**: `ts_check` finds no problem.

Then it starts the next round on the same file. Rounds alternate between
closing the database cleanly and abandoning it, so a round can start with a
non-empty log.

```
round  58: killed after #854 (+1 in flight) -> recovered #855, 2471 keys, 992 pages, 0 WAL pages: ok
round  59: killed after #866 (+1 in flight) -> recovered #867, 2491 keys, 998 pages, 70 WAL pages: ok
round  60: killed after #883 (+1 in flight) -> recovered #884, 2513 keys, 1034 pages, 43 WAL pages: ok
crash test passed: 60 kills, 884 transactions, 48 in-flight commits survived the kill
```

A killed process cannot lose data that the OS has already accepted. Losing
the disk cache is what the power-loss tests above simulate.

## Performance

`tessera_bench -n 200000` writes 200,000 keys with 100-byte values, with
`sync=full`, writes batched 1000 per transaction, and an 8 MiB cache for a
49 MiB database:

| Phase                          | Linux (Docker, GCC -O2) | Windows 11 (MSVC /O2) |
|--------------------------------|------------------------:|----------------------:|
| insert, sequential keys        | 150,000 ops/s           | 239,000 ops/s         |
| get, random keys               | 319,000 ops/s           | 116,000 ops/s         |
| scan, keys and values          | 4,355,000 ops/s         | 2,242,000 ops/s       |
| update, random keys            | 35,000 ops/s            | 14,500 ops/s          |
| delete, random keys            | 30,000 ops/s            | 12,200 ops/s          |
| put, one fsync'd commit each   | 209 ops/s               | 485 ops/s             |
| integrity check                | 290,000 pages/s         | 151,000 pages/s       |

With `--cache 20000` (the whole database in memory), random gets on Windows
rise to about 717,000 ops/s and the check to 1.5 million pages/s. The numbers
depend heavily on the disk and the OS, so measure on your own machine:

```bash
build/tessera_bench -n 200000
build/tessera_bench -n 1000000 --value 200 --sync normal --cache 65536
```

How to read them:

- **Single-commit puts** are bound by fsync latency: that is the cost of
  durability, and the reason to group writes into transactions.
- **Random updates and deletes** touch a different leaf for almost every key,
  so each 1000-key transaction logs about 1000 pages (4 MiB), and the
  checkpoint writes them again. Sequential inserts fill one leaf at a time and
  log far fewer pages.
- **Random gets** are bound by cache misses once the database outgrows the
  cache. Each miss is one 4 KiB read plus a checksum.

## Project layout

```
include/tessera.h         public API
src/util/                 CRC32C (slicing-by-8), hash map, status strings
src/os/                   VFS: POSIX, Win32, in-memory with crash simulation
src/storage/              write-ahead log, pager, free list
src/btree/                node format, B+tree, overflow pages, cursors
src/engine/               database handle, transactions, integrity checker
src/query/                lexer, parser, executor
tools/                    CLI, crash test, benchmark, deterministic workload
tests/                    test runner and tests
docs/FORMAT.md            on-disk format
```

## Pushing to GitHub

1. Create an **empty** repository on GitHub named `Tessera` (no README,
   .gitignore or license, because they already exist here).
2. From this folder:

   ```bash
   cd C:\VSCode\ProiecteVSCode\Tessera
   git init -b main
   git add .
   git commit -m "Tessera: crash-safe key-value store with a B+tree, WAL and query language"
   git remote add origin https://github.com/YUStulinu/Tessera.git
   git push -u origin main
   ```

3. Later changes:

   ```bash
   git add .
   git commit -m "Describe the change"
   git push
   ```

The `.gitignore` keeps `build/` and test databases (`*.tdb`, `*.tdb-wal`) out
of the repository. The GitHub Actions workflow
(`.github/workflows/ci.yml`) builds and tests the project on every push:

- GCC and Clang on Linux, Clang on macOS, and MSVC on Windows, with warnings
  treated as errors;
- the sanitizers and valgrind;
- 150 crash-test kills per platform.

## License

MIT - see [LICENSE](LICENSE).
