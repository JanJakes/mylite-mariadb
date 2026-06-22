# Embedded InnoDB Temporary Rollback Segment Pool Size

## Problem

The temp rollback-segment attribution slice showed that process-style warm opens
still pay about 2.1 ms creating temporary rollback segment headers. Almost all
of that time is inside `trx_rseg_header_create()` across the fixed 128
temporary rollback segment slots, even though MyLite's embedded profile does
not need a daemon-scale temporary-table undo distribution on every process
startup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0types.h` defines
  `TRX_SYS_N_RSEGS` as 128 because rollback segment ids must fit in the 7 bits
  reserved in `DB_ROLL_PTR`.
- `mariadb/storage/innobase/trx/trx0sys.cc:trx_sys_t::create()` initializes all
  `trx_sys.temp_rsegs` entries as empty in-memory rollback segment objects.
- `mariadb/storage/innobase/trx/trx0rseg.cc:trx_temp_rseg_create()` then
  creates native temp-space rollback segment header pages and reinitializes the
  corresponding in-memory entries.
- `mariadb/storage/innobase/trx/trx0trx.cc:trx_t::assign_temp_rseg()` chooses
  temp rollback segments by round-robin masking and only needs a power-of-two
  active pool.
- The attribution sample in
  `../embedded-innodb-temp-rseg-startup-attribution/specs.md` reported two
  ordinary warm opens creating 256 temp rollback segments with `2.007 ms` per
  open in header creation, and two ownerless warm opens creating 256 segments
  with `2.039 ms` per open in header creation.

## Design

Keep the upstream 128-entry arrays, rollback segment id width, persistent
rollback segment behavior, and temp rollback segment data structures unchanged.
For the embedded profile, create and assign a 16-entry power-of-two prefix:

- add `MYLITE_EMBEDDED_TEMP_RSEGS=16` alongside `TRX_SYS_N_RSEGS`;
- keep the invariant that the embedded temp pool is no larger than the upstream
  array and is a power of two;
- have `trx_temp_rseg_create()` create header pages only for that prefix; and
- have `trx_t::assign_temp_rseg()` round-robin only within that same prefix.

This reduces startup header creation while keeping temp-table transactions on
initialized native temp rollback segments. Unused array entries remain
initialized empty objects and are still destroyed by the existing shutdown loop.

## Scope And Non-Goals

In scope:

- Embedded temporary rollback segment create/assignment pool size.
- Ordinary InnoDB temporary-table DML coverage.
- Ownerless temporary-table stress verification.
- Startup probe evidence that created temp rollback segments fall from 128 per
  open to 16 per open.

Out of scope:

- Changing persistent rollback segment count, `TRX_SYS_N_RSEGS`, undo
  tablespaces, durable undo behavior, or rollback pointer format.
- Lazy temp rollback segment creation.
- Runtime configuration for temp rollback segment count.
- Changing temp tablespace placement, sparse sizing, cleanup, or ownerless
  coordination protocols.

## Compatibility Impact

SQL semantics, public C API behavior, PHP mysqli behavior, and on-disk durable
formats are unchanged. The observable compatibility tradeoff is performance
distribution for workloads with more than 16 concurrent temp-table writers:
those writers share 16 temp rollback segments instead of 128. Correctness still
uses native InnoDB latching and undo pages.

## Directory And Native Storage Impact

No durable database-directory format changes. The temporary tablespace remains
non-durable and reinitialized on startup. Fewer temporary rollback segment
header pages are created in the temp tablespace.

## Binary Size, License, And Dependency Impact

The slice changes MariaDB-derived InnoDB startup and transaction assignment
code plus a first-party embedded API test. It adds no dependency and has
negligible binary-size impact.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe`, `mylite_embedded_exec_test`, and
  `mylite_embedded_open_close_test` with the production PHP embedded preset.
- Run focused embedded exec coverage for ordinary temporary-table DML.
- Run focused embedded open/close lifecycle coverage.
- Run ownerless temporary-table stress.
- Run a reduced production performance probe and verify:
  - ordinary and ownerless warm opens complete;
  - temp rollback segment created count is 16 per successful
    `trx_temp_rseg_create()` call; and
  - temp rollback segment header time drops from the 128-segment attribution
    baseline.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Startup creates only 16 embedded temporary rollback segments per open.
- Temp rollback segment assignment never selects an uncreated slot.
- Ordinary InnoDB temporary-table insert/update/select/drop works.
- Ownerless cross-process temporary-table stress passes.
- Reduced production probe records lower temp rollback-segment header time.

## Verification Results

The slice is verified with:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target mylite_embedded_performance_probe mylite_embedded_exec_test mylite_embedded_open_close_test mylite_ownerless_cross_process_sql_test`
- `ctest --preset php-embedded-prod -R '^libmylite\.embedded-exec$' --output-on-failure`
- `ctest --preset php-embedded-prod -R '^libmylite\.embedded-open-close$' --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test temp-stress`
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

- ordinary warm open/close `103.082 ms`;
- ordinary temp tablespace `total=0.380 ms`, `rseg_create=0.246 ms`,
  `created_count=32` across two opens, and temp rseg header creation
  `0.238 ms` per open;
- ownerless warm open/close `86.974 ms`; and
- ownerless temp tablespace `total=0.451 ms`, `rseg_create=0.320 ms`,
  `created_count=32` across two opens, and temp rseg header creation
  `0.309 ms` per open.

Compared with the immediately preceding 128-segment attribution sample,
ordinary warm-open temp rseg header creation dropped from `2.007 ms` to
`0.238 ms` per open, and ownerless warm-open header creation dropped from
`2.039 ms` to `0.309 ms` per open. The created count confirms 16 temp rollback
segments per successful temporary tablespace startup.

## Risks

- Heavy same-process temp-table write workloads with more than 16 concurrent
  temp-table writers may see more rollback-segment latch sharing than upstream
  MariaDB. This is an embedded startup-performance tradeoff and should be
  revisited if application evidence shows temp-table writer contention.
- This intentionally does not address the larger clean redo scan startup cost.
