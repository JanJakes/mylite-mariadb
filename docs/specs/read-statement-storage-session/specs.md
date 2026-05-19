# Read Statement Storage Session

## Problem

Point-select profiling after the indexed-rowset builder slice shows that the
primary-key read path still spends a large share of time reopening the primary
`.mylite` file, probing pending journals, taking a shared lock, decoding the
header, and validating the catalog root for each storage helper call.

That is correct but not SQLite-like. The handler already has a bounded cursor
construction phase where it finds matching index row ids and materializes the
selected row payloads. MyLite should keep the storage read handle and validated
root pages hot across that phase instead of rebuilding the file view for every
storage helper call.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` can call multiple
  durable helpers for one point lookup: exact index lookup, entryset lookup,
  and selected row materialization.
- Durable read helpers call `open_existing_file()` independently, so a primary
  key point lookup currently reopens and revalidates the same `.mylite` file
  more than once.
- `packages/mylite-storage/src/storage.c` already has active-statement fast
  paths for header and catalog reads, but they only apply to write-capable
  checkpoints.
- The storage layer already has bounded snapshot machinery for same-thread
  active writers and cross-process transaction journals. A read-only statement
  session can use the same validated header/catalog snapshot shape without
  changing durable format.

## Design

- Add a first-party storage read-statement session API:
  - `mylite_storage_begin_read_statement(filename, &statement)`;
  - `mylite_storage_end_read_statement(statement)`.
- A read statement:
  - opens the primary file in a read-capable mode;
  - performs the existing pending-journal recovery path;
  - takes a shared advisory lock;
  - reads and validates the header and catalog root once;
  - keeps the file handle and lock until the read statement ends.
- `open_existing_file()` reuses the active same-owner read statement before
  falling back to per-call open and lock work.
- `read_header()`, `read_page_at()`, and `read_catalog_root()` return the
  cached header/catalog pages for the active read statement.
- If a same-owner write checkpoint is already active, the read-statement begin
  call succeeds as a no-op because storage reads should use the write
  checkpoint's current file view.
- `ha_mylite::build_index_cursor()` starts a scoped read session around durable
  cursor construction and ends it before returning to MariaDB. This avoids
  broad table-lock lifetime changes for joins and `CREATE TABLE ... SELECT`
  while still covering the measured primary-key and secondary exact-read hot
  paths.

## Compatibility Impact

No SQL-visible behavior change is intended. A single durable cursor build sees
one validated header/catalog view while it constructs the cursor and selected
row payload batch. Concurrent writers already conflict with shared readers
through advisory locks; the change extends that lock only across the scoped
cursor build, not across whole SQL statement table-lock lifetimes.

The read session does not add MVCC, WAL, gap locks, or isolation-level-specific
read views. It is a statement-local file-handle/cache lifetime optimization.

## Single-File And Lifecycle Impact

No durable file-format change. The primary `.mylite` file remains the only
durable database file. No new companion file is introduced. The read session
only keeps a process-local file handle and shared lock alive until statement
unlock.

## Public API And File-Format Impact

The first-party storage C API gains read-statement begin/end functions. The
public `libmylite` API and on-disk format do not change.

## Storage-Engine Routing Impact

Durable routed tables benefit regardless of requested engine spelling:
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted engine, and explicit
`ENGINE=MYLITE` all reach the MyLite handler. Runtime-volatile MEMORY/HEAP rows
do not use this durable file-handle path.

## Tests And Verification

- Add storage coverage proving a read statement holds a shared lock: a different
  owner cannot write while it is active, and can write after it ends.
- Add handler/libmylite smoke coverage through the existing storage-engine
  harness and performance baseline.
- Run storage unit tests, storage-engine compatibility coverage, performance
  baseline, formatting, and whitespace checks.

Local verification:

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke TARGET=mysqlserver tools/mariadb-embedded-build build`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.cc mariadb/storage/mylite/ha_mylite.h`
- `git diff --check`

Performance evidence from the verified 1000-row, 10000-iteration local baseline:
direct primary-key point selects are `86.786 us/op`, prepared primary-key point
selects are `65.325 us/op`, direct published-leaf secondary exact selects are
`119.906 us/op`, and prepared published-leaf secondary exact selects are
`92.472 us/op`.

## Acceptance Criteria

- Repeated durable storage calls inside one routed index cursor build reuse one
  read-only file handle.
- Header and catalog root validation happens once at read-statement start, not
  once per point lookup helper.
- Existing write checkpoint behavior and transaction rollback semantics remain
  unchanged.
- The performance baseline shows improved point-select and exact-index read
  timings without regressing correctness checks.
