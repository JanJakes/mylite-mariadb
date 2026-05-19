# Offset Page I/O

## Problem

Hot point and secondary index reads still spend measurable time inside fixed
page reads after the read-file and checkpoint snapshot caches hit. The current
random page path uses `fseek()` followed by `fread()`, which adds stdio file
position changes, stdio refill work, and stdio locking around every durable
page read.

The storage layer already treats durable pages as fixed-size pages addressed by
page id. Random page reads and writes do not need the stream position side
effects that `fseek()` / `fread()` / `fwrite()` provide.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::read_page_at()` serves durable
  header, catalog, row, row-state, index, autoincrement, and journal page
  reads. Active statement and snapshot fast paths return in-memory pages before
  the physical read path.
- `packages/mylite-storage/src/storage.c::write_page_at_raw()` publishes random
  primary-file pages for header, catalog, row, row-state, index, journal
  restore, and tail publication paths.
- Sequential page writes are limited to new database creation and rollback
  journal creation through `write_page()`. They still rely on stream order and
  are not part of this slice.
- Recent local samples of repeated exact index reads show `read_page_at()` time
  below `read_cached_checkpoint_snapshot()`, including `fseek()`, `fread()`,
  `__sread`, and kernel `read` calls.

## Design

- Keep all existing active statement, read-statement, read-snapshot, and
  transaction-journal snapshot fast paths.
- Replace the physical random read path in `read_page_at()` with a helper that
  reads exactly one page through `pread()` at the computed page offset.
- Replace the physical random write path in `write_page_at_raw()` with a helper
  that writes exactly one page through `pwrite()` at the computed page offset.
- Retry interrupted system calls and treat short reads as corruption.
- Leave sequential `write_page()` unchanged for empty-file initialization and
  journal construction.

## Compatibility Impact

No SQL or API behavior changes. The handler and storage APIs keep the same
locking, recovery, snapshot, checksum, and corruption behavior.

## Single-File And Lifecycle Impact

No file-format or companion-file change. The implementation still uses the same
primary `.mylite` file and existing MyLite-owned transient journals.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

All durable routed engines benefit through the existing MyLite handler path,
including `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted engine, and
`ENGINE=MYLITE`. Runtime-volatile `MEMORY` / `HEAP` tables do not use durable
page I/O.

## Binary-Size And Dependency Impact

No new dependency. `pread()` and `pwrite()` are POSIX calls already available
through the storage layer's existing `<unistd.h>` include.

## Test And Verification Plan

- Run storage unit tests to cover random reads/writes, row DML, index DML,
  rollback journals, crash recovery, and corruption checks.
- Rebuild storage-smoke targets and run the storage-engine compatibility
  harness.
- Run the local performance baseline and compare point-select and exact
  secondary-select timings.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Random page reads no longer call `fseek()` / `fread()` in the physical path.
- Random page writes no longer call `fseek()` / `fwrite()` in the physical path.
- Short reads still produce corruption errors rather than partially initialized
  pages.
- Existing storage and storage-engine compatibility tests pass.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 100000`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- `git diff --check`

Local performance sample after implementation:

- direct primary-key point selects: `26.317 us/op`
- prepared primary-key point selects: `9.723 us/op`
- direct secondary exact selects: `63.676 us/op`
- prepared secondary exact selects: `38.350 us/op`
- direct published-leaf secondary exact selects: `64.943 us/op`
- prepared published-leaf secondary exact selects: `38.928 us/op`
- direct primary-key updates in one transaction: `144.614 us/op`
- prepared primary-key updates in one transaction: `178.675 us/op`
- direct ordered full scan: `2940.402 us/op`

## Risks

- The storage layer still uses `FILE *` handles because surrounding code owns
  stdio open/close and sequential journal writes. Random page I/O must not rely
  on stream position after this slice.
- Mixing offset I/O with sequential stdio writes remains safe only because
  sequential writes are limited to newly created files and journals, while
  random primary-file publication uses the offset helpers.
