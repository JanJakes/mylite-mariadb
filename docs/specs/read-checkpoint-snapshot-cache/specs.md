# Read Checkpoint Snapshot Cache

## Problem

After scoped read sessions, prepared primary-key point-select profiling still
spends most sampled storage time starting a read session for each SQL statement.
The dominant storage costs are reopening/probing the primary file and repeatedly
decoding and checksumming the same header and catalog root pages.

SQLite-like point-read behavior needs a file-runtime view that stays hot across
statements on one open MyLite database handle. The first bounded step is to
reuse a previously validated decoded checkpoint snapshot when the raw durable
header and catalog bytes are unchanged.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- A 2026-05-19 `sample` run over
  `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 100000` shows
  prepared point-select execution under
  `ha_mylite::build_index_cursor()` dominated by
  `mylite_storage_begin_read_statement()`,
  `initialize_read_statement()`, `read_checkpoint_snapshot()`,
  `decode_header_page()`, `validate_catalog_root_bytes()`, `checksum_page()`,
  `fopen()`, and `recover_pending_journals()`.
- The existing read-statement session validates header/catalog state once per
  cursor build, but every SQL statement still opens a new read session.
- The durable header page changes for committed row/index mutations because
  page-count publication is append-only. Catalog mutations also change the
  catalog page bytes and usually catalog generation.

## Design

- Add a thread-local, owner-scoped, single-file checkpoint snapshot cache in the
  storage layer.
- On read-statement start after pending-journal recovery and shared-lock
  acquisition:
  - read the raw header page;
  - if it matches the cached raw header page, read the cached catalog-root page
    id and compare the raw catalog page;
  - if both raw pages match, copy the cached decoded header/catalog state into
    the read statement without rerunning header/catalog checksums;
  - otherwise decode and validate the header/catalog pages normally and replace
    the cache with the newly validated snapshot.
- Keep this cache process-local and advisory. It does not replace locks,
  journals, recovery, or future pager state.
- Replace journal existence probes that only need path existence with `stat()`
  instead of opening the journal files.

## Compatibility Impact

No SQL-visible behavior change is intended. The cache is used only after the
storage layer reads and compares the durable raw header and catalog bytes while
holding the same shared lock the read statement already took.

## Single-File And Lifecycle Impact

No file-format change and no new companion file. The cache is transient
thread-local process memory keyed by owner and primary filename. If the bytes do
not match, the storage layer falls back to full validation.

## Public API And File-Format Impact

No public `libmylite` API or file-format change. The storage C API remains the
same as the scoped read-session slice.

## Storage-Engine Routing Impact

Routed durable engines benefit through the existing MyLite handler path:
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted engine, and
`ENGINE=MYLITE`. Runtime-volatile `MEMORY` / `HEAP` tables remain outside this
file-backed path.

## Tests And Verification

- Add storage unit coverage proving a second read statement over an unchanged
  file reuses the cached snapshot and still sees newly committed writes after
  the header/catalog bytes change.
- Run storage unit tests, storage-engine compatibility coverage, performance
  baseline, formatting, and whitespace checks.

Local verification:

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke TARGET=mysqlserver tools/mariadb-embedded-build build`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

Performance evidence from the verified 1000-row, 10000-iteration local baseline:
direct primary-key point selects are `75.144 us/op`, prepared primary-key point
selects are `53.497 us/op`, direct published-leaf secondary exact selects are
`108.852 us/op`, and prepared published-leaf secondary exact selects are
`84.860 us/op`.

## Acceptance Criteria

- Repeated read statements over unchanged durable header/catalog pages avoid
  repeated header/catalog checksum validation.
- Any header or catalog byte change falls back to full validation and updates
  the cache.
- Existing recovery, locking, transaction snapshot, and handler smoke coverage
  remains green.
- The performance baseline shows lower primary-key point-select startup cost
  without introducing persistent sidecars or broader lock lifetimes.
