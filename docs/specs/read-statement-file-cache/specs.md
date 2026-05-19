# Read Statement File Cache

## Problem

After scoped read sessions and checkpoint snapshot reuse, sampled secondary
exact-select work still spends a large share of cursor-build time opening the
primary `.mylite` file for each read statement:

- `ha_mylite::build_index_cursor()`
- `mylite_storage_begin_read_statement()`
- `initialize_read_statement()`
- `fopen()`
- `lock_file()`
- `read_cached_checkpoint_snapshot()`

The current read-statement session correctly releases its shared lock and closes
the file at the end of each cursor build. That keeps lock lifetimes narrow, but
it also redoes `fopen()` work for every hot point or exact secondary select.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` starts a storage
  read statement around durable cursor construction.
- `packages/mylite-storage/src/storage.c::initialize_read_statement()` performs
  pending-journal recovery checks, opens the primary file with `fopen("r+b")`,
  takes a shared advisory lock, then reads the cached checkpoint snapshot.
- `mylite_storage_end_read_statement()` closes the owned file handle through
  `close_statement()`.
- `open_existing_file_for_update()` can temporarily upgrade a same-owner active
  read statement's file lock to exclusive for writes, so any read-file reuse
  must release the lock before caching the file handle.
- Existing read-statement storage coverage proves a different owner is blocked
  while a read statement is active and can write after it ends.

## Design

- Add a thread-local single-entry read-file cache for normal durable read
  statements.
- The cache stores an unlocked `FILE *` plus the filename string. It does not
  store a validated read view.
- On normal read-statement startup after pending-journal recovery succeeds:
  - try to reuse a cached file only when the filename matches;
  - compare `stat(filename)` with `fstat(fileno(cached_file))` before reuse so
    external unlink/replace operations do not make MyLite read stale inodes;
  - take the usual shared lock;
  - run the existing cached checkpoint snapshot path.
- On read-statement end:
  - if the statement owns a normal read file, release any shared or upgraded
    advisory lock with `LOCK_UN`;
  - keep the unlocked file in the thread-local cache for later read statements.
- Do not cache transaction-journal snapshot files, active write checkpoint
  files, nested read-statement borrowed files, or files from failed read
  statement startup.
- Clear the cached read file when creating or mutating the same filename so a
  write path does not leave stdio buffers or stale path handles behind.

## Compatibility Impact

No SQL-visible behavior should change. Every read statement still performs
pending-journal checks, takes the normal shared lock, and validates or reuses
the checkpoint snapshot only after reading durable page bytes.

External file replacement preserves existing filename semantics because cached
handles are reused only when the cached file descriptor still matches the
current path's device and inode.

## Single-File And Lifecycle Impact

No file-format change and no companion file. The cache is transient
thread-local process state and holds no lock between read statements.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

Durable routed tables benefit through the existing MyLite handler path:
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted engine, and
`ENGINE=MYLITE`. Runtime-volatile `MEMORY` / `HEAP` tables do not use the
primary file read-session path.

## Binary-Size And Dependency Impact

No new dependency. The implementation uses existing C/POSIX file APIs already
used by the storage layer.

## Test And Verification Plan

- Extend storage read-statement coverage to prove a writer can still write
  after a read statement ends, covering lock release before file caching.
- Add storage coverage for replacing a file path after a cached read statement
  so later reads use the new file, not the stale cached descriptor.
- Run storage unit tests.
- Rebuild storage-smoke targets and run the storage-engine compatibility
  harness.
- Run the local performance baseline and compare point-select and exact
  secondary-select timings.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Normal repeated read statements avoid repeated `fopen()` when the same path
  still refers to the same file.
- Cached files are unlocked between read statements and do not block later
  writers.
- Replaced paths are detected and not served from stale cached descriptors.
- Storage and storage-engine compatibility tests pass.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

Local perf sample after implementation, second run:

- direct primary-key point selects: `42.809 us/op`
- prepared primary-key point selects: `21.804 us/op`
- direct secondary exact selects: `82.940 us/op`
- prepared secondary exact selects: `58.400 us/op`
- direct published-leaf secondary exact selects: `76.742 us/op`
- prepared published-leaf secondary exact selects: `49.127 us/op`

## Risks

- This still performs pending-journal existence checks and advisory lock
  acquisition per read statement. A full SQLite-like pager needs a broader
  file-runtime design.
- The cache is intentionally one entry and thread-local. Workloads alternating
  many files will fall back to normal open/close behavior.
