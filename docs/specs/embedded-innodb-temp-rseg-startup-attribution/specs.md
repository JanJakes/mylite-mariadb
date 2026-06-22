# Embedded InnoDB Temporary Rollback Segment Startup Attribution

## Problem

After sparse sizing removed the repeated 12 MiB temporary tablespace file
allocation from process-style warm opens, the remaining temporary tablespace
startup bucket is temporary rollback-segment creation. The existing probe only
reported the whole `trx_temp_rseg_create()` call, so it could not show whether
the cost was mini-transaction setup, rollback segment header creation, in-memory
segment reset, or mini-transaction commit work.

MyLite needs that split before changing native InnoDB temporary rollback
segment behavior. The path is paid by PHPUnit-style process isolation and by
ownerless open/close probes, so it is a high-signal startup target.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/srv/srv0start.cc:srv_open_tmp_tablespace()`
  initializes the temporary tablespace header with `MTR_LOG_NO_REDO`, then
  calls `trx_temp_rseg_create()` before returning from InnoDB system-table
  startup.
- `mariadb/storage/innobase/trx/trx0rseg.cc:trx_temp_rseg_create()` loops over
  `trx_sys.temp_rsegs`, starts a no-redo mini-transaction, X-locks the temp
  space, creates a rollback segment header with `trx_rseg_header_create()`,
  destroys and reinitializes the in-memory `trx_rseg_t`, and commits the
  mini-transaction for each slot.
- `mariadb/storage/innobase/include/trx0types.h` defines
  `TRX_SYS_N_RSEGS` as 128.
- `mariadb/storage/innobase/trx/trx0trx.cc:trx_t::assign_temp_rseg()` chooses
  temporary rollback segments by round-robin masking across all
  `TRX_SYS_N_RSEGS` slots and asserts the selected segment is not persistent.
  Reducing or lazily creating slots therefore needs a separate transaction
  assignment design and concurrency proof.

## Design

Keep the MariaDB temporary rollback segment creation order unchanged and add
disabled-by-default startup counters inside `trx_temp_rseg_create()`:

- function call count;
- setup time for mini-transaction start, no-redo mode, and temp-space X-lock;
- header creation time for `trx_rseg_header_create()`;
- in-memory `trx_rseg_t::destroy()` plus `trx_rseg_t::init()` time;
- mini-transaction commit time; and
- successful created-segment count.

Expose these values in both compact `mylite_perf_summary_*` output and detailed
`mylite_perf_*_startup_phase_*` output. The compact averages use
`trx_temp_rseg_create()` calls as the denominator so the CI-visible values stay
per embedded open. The detailed output also includes the created segment count,
which should normally be 128 per successful temp tablespace startup.

## Scope And Non-Goals

In scope:

- Startup performance attribution for temporary rollback segment creation.
- Production probe output for ordinary embedded and ownerless warm opens.
- Documentation of the current 128-slot native dependency.

Out of scope:

- Reducing `TRX_SYS_N_RSEGS`.
- Lazy temporary rollback segment creation.
- Changing `trx_t::assign_temp_rseg()`, transaction assignment, purge, undo, or
  recovery behavior.
- Changing temporary tablespace cleanup, sparse sizing, header initialization,
  redo, or ownerless coordination.

## Compatibility Impact

No SQL behavior, public C API behavior, PHP mysqli behavior, file format,
temporary tablespace lifecycle, or ownerless locking behavior changes. The slice
adds instrumentation only.

## Directory And Native Storage Impact

No database-directory layout or durable storage format changes. Temporary
rollback segment header pages are still created through the same no-redo native
InnoDB path on each temporary tablespace startup.

## Binary Size, License, And Dependency Impact

The slice touches MariaDB-derived InnoDB startup code and first-party probe
mirrors. It adds no dependency and only a few internal enum slots and counter
emissions when startup attribution is enabled.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` and
  `mylite_embedded_open_close_test` with the production PHP embedded preset.
- Run focused embedded open/close lifecycle coverage.
- Run a reduced production performance probe and verify:
  - `innodb_temp_rseg_create_calls` is positive for warm opens;
  - `innodb_temp_rseg_create_created_count` is 128 per successful
    `trx_temp_rseg_create()` call; and
  - setup, header, memory, and commit timing keys are emitted.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Probe output splits `trx_temp_rseg_create()` into setup, header creation,
  memory reset, and commit timing.
- Probe output exposes the temporary rollback segment creation count.
- Ordinary embedded and ownerless warm opens still complete.
- Documentation records why reducing the 128-slot path is a separate slice.

## Verification Results

The slice is verified with:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target mylite_embedded_performance_probe mylite_embedded_open_close_test`
- `ctest --preset php-embedded-prod -R '^libmylite\.embedded-open-close$' --output-on-failure`
- reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2 MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=1 MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=1 build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`

The reduced probe reported:

- ordinary warm open/close `107.511 ms`;
- ordinary temp tablespace `total=2.216 ms` and
  `rseg_create=2.091 ms`;
- ordinary temp rollback segment creation `calls=2`,
  `created_count=256`, `setup=0.015 ms`, `header=2.007 ms`,
  `memory=0.015 ms`, and `commit=0.030 ms`;
- ownerless warm open/close `109.556 ms`;
- ownerless temp tablespace `total=2.250 ms` and
  `rseg_create=2.117 ms`; and
- ownerless temp rollback segment creation `calls=2`,
  `created_count=256`, `setup=0.014 ms`, `header=2.039 ms`,
  `memory=0.010 ms`, and `commit=0.031 ms`.

The measured remaining cost is therefore rollback segment header creation, not
mini-transaction setup, in-memory `trx_rseg_t` reset, or commit work. The
created count confirms 128 temporary rollback segments per successful temporary
tablespace startup.

The follow-up `../embedded-innodb-temp-rseg-pool-size/specs.md` slice reduces
the embedded temp rollback segment create and assignment pool while preserving
the upstream 128-entry arrays and durable rollback segment behavior.

## Risks

- This does not reduce the remaining startup cost by itself. It is intentionally
  an attribution slice because the direct count-reduction shortcut would touch
  transaction assignment and native undo semantics.
- Counter overhead is only present when startup attribution is enabled by the
  performance probe.
