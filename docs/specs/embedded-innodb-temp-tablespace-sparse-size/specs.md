# Embedded InnoDB Temporary Tablespace Sparse Size

## Problem

Production startup attribution shows process-style MyLite opens are still
paying native InnoDB temporary tablespace creation work on every warm open.
The temporary tablespace is deleted and recreated during startup, and the
dominant child is `SysTablespace::set_size()` physically sizing a fresh 12 MiB
`ibtmp1` file before InnoDB immediately reinitializes the temp tablespace
header and temporary rollback segment.

That cost affects process-isolated PHPUnit and open/close probes, but it is
not durable database state and does not need the same preallocation behavior as
InnoDB system tablespace files.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/srv/srv0start.cc:srv_start()` calls
  `srv_open_tmp_tablespace(create_new_db)` after InnoDB system-table startup
  and before the master timer.
- `mariadb/storage/innobase/srv/srv0start.cc:srv_open_tmp_tablespace()`
  deletes leftover temp files, checks the temp file spec with a 12 MiB minimum,
  calls `srv_tmp_space.open_or_create(true, create_new_db, ...)`, opens the
  resulting temp space through the fil system, initializes the header with
  `MTR_LOG_NO_REDO`, and creates the temporary rollback segment.
- `mariadb/storage/innobase/fsp/fsp0sysspace.cc:SysTablespace::set_size()`
  currently calls `os_file_set_size()` without the sparse flag for all
  system-tablespace users. Its comment describes physically writing the new
  file full of zeros.
- `mariadb/storage/innobase/os/os0file.cc:os_file_set_size(..., bool
  is_sparse)` already supports sparse sizing on non-Windows builds by using
  `ftruncate()` when the sparse flag is true. The Windows inline overload
  ignores the sparse flag and preserves existing behavior.
- Existing attribution in
  `docs/specs/embedded-innodb-temp-tablespace-startup-attribution/specs.md`
  reported repeated temp tablespace create-new calls, zero reuse calls, and
  about 8-10 ms per open in the temp `open_or_create()` bucket.

## Design

Keep InnoDB's temp tablespace lifecycle and initialization order unchanged, but
size newly created temporary tablespace files sparsely:

- pass the `is_temp` flag from `SysTablespace::open_or_create()` into the
  created-file path;
- call the existing sparse `os_file_set_size()` overload only for
  non-raw files created for the temporary tablespace;
- leave system tablespace, undo, redo, user tablespace, raw-file, and existing
  file sizing behavior unchanged;
- keep `srv_open_tmp_tablespace()` header initialization and temporary rollback
  segment creation unchanged; and
- add a startup perf counter for temp sparse-size calls so production probes
  can prove the optimized path was active without relying on timing thresholds.

## Scope And Non-Goals

In scope:

- Sparse sizing for newly created InnoDB temporary tablespace data files.
- Startup perf counter and probe output showing sparse temp sizing.
- Ordinary embedded and ownerless open/close verification.

Out of scope:

- Changing `innodb_temp_data_file_path`, the 12 MiB minimum, temp tablespace
  directory placement, cleanup policy, read-only startup, or ownerless
  concurrency file protocols.
- Sparse sizing for durable InnoDB files.
- Skipping temp header initialization or temporary rollback-segment creation.
- Broader clean redo scan, recovery bootstrap, or DDL/file-lifecycle recovery
  work.

## Compatibility Impact

SQL behavior, public C API behavior, PHP mysqli behavior, and MySQL/MariaDB
compatibility claims are unchanged. Temporary tablespace contents are
non-durable and reinitialized on startup. Filesystem allocation is deferred for
new temp-table pages rather than forced during process startup.

## Directory And Native Storage Impact

No file names, directory layout, durable WAL/checkpoint format, or native
InnoDB on-disk application table format changes. The same temp tablespace file
is created, opened, initialized, and removed through the existing lifecycle.

## Binary Size, License, And Dependency Impact

The slice changes first-party instrumentation and a small MariaDB-derived
InnoDB startup path. It adds no dependency and has negligible binary-size
impact.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` and
  `mylite_embedded_open_close_test` with the production PHP embedded preset.
- Run focused embedded open/close lifecycle coverage.
- Run a reduced production performance probe and verify:
  - `innodb_temp_tablespace_sparse_set_size_calls` is positive for warm opens;
  - create-new calls are still reported; and
  - ordinary and ownerless warm open/close still complete.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- New temporary tablespace files use sparse sizing on non-Windows builds.
- Durable InnoDB file sizing still uses the existing non-sparse path.
- Startup probe output exposes sparse temp sizing.
- Focused embedded lifecycle and production guard checks pass.

## Verification Results

The slice is verified with:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target mylite_embedded_performance_probe mylite_embedded_open_close_test`
- `ctest --preset php-embedded-prod -R '^libmylite\.embedded-open-close$' --output-on-failure`
- reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2 MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=1 MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=1 build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R 'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace' --output-on-failure`
- `ctest --preset php-embedded-prod -R '^libmylite\.embedded-transactions-recovery$' --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced probe reported:

- ordinary warm open/close `144.681 ms`;
- ordinary temp tablespace `total=2.240 ms`, `open_or_create=0.084 ms`,
  `sparse_set_size_calls=2`, `create_new_calls=2`, and
  `reuse_existing_calls=0`;
- ownerless warm open/close `139.006 ms`; and
- ownerless temp tablespace `total=2.384 ms`, `open_or_create=0.065 ms`,
  `sparse_set_size_calls=2`, `create_new_calls=2`, and
  `reuse_existing_calls=0`.

The remaining temp tablespace startup cost is temporary rollback-segment
creation (`2.102 ms` ordinary, `2.263 ms` ownerless in the same reduced
sample), not physical 12 MiB temp-file sizing.

## Risks

- Sparse temp sizing can defer ENOSPC from startup to later temporary
  tablespace writes. That is acceptable for non-durable temporary storage but
  is intentionally not applied to durable files.
- The measured wall-time gain is filesystem dependent. The correctness signal
  for this slice is the sparse-size counter and unchanged lifecycle behavior,
  not a hard timing threshold.
