# Ownerless Redo Completed Range Count

## Problem

The ownerless redo written/leave fusion and active-reservation count slices
reduced progress-latch passes and active reservation scans, but the normal
sequential redo completion path still called `drain_completed_ranges()` after
every contiguous range advance. `drain_completed_ranges()` scanned the whole
completed-range table even when no out-of-order completed range existed.

Stats-enabled production bulk attribution continued to show visible redo-leave
time in the 100-row bulk path, so the empty completed-range scan was a bounded
hot-path target.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/mtr/mtr0mtr.cc` is the native mini-transaction
  commit path that MyLite's ownerless redo callbacks wrap for page-version
  publication and written/latest LSN coordination.
- `packages/libmylite/src/ownerless_redo_state.cc` stores redo latest,
  reserved, written, visible, active reservation, and completed-range state in
  the directory-backed redo-state shared-memory segment.
- `complete_write_locked()` already serializes redo written-range updates
  under the redo progress latch, then calls `drain_completed_ranges()` after
  contiguous completion.
- `record_completed_range()` writes nonzero-start completed-range slots only
  for out-of-order ranges; sequential ownerless writes commonly have no such
  slots.
- Offset `92` in the redo-state segment was unused between the active
  reservation count at offset `88` and the progress latch at offset `96`.

## Design

Store a maintained `uint32_t` completed-range count at redo-state offset `92`:

- redo-state shared-memory segment metadata moves from version `9` to `10`;
- out-of-order range recording increments the count after publishing a merged
  slot;
- merging and draining decrement the count when clearing a slot;
- `drain_completed_ranges()` returns immediately when the count is zero;
- snapshot diagnostics expose the count for primitive tests.

The count is protected by the existing redo progress latch for writers. It is
an internal acceleration only; it does not change LSN ordering, completed-range
coalescing rules, page-version WAL, checkpoint files, native InnoDB formats, SQL
behavior, or public C API behavior.

## Compatibility Impact

The segment version bump causes old redo-state `.shm` metadata to be rebuilt
instead of interpreted with the new offset. Durable database files,
`mylite-concurrency.wal`, `mylite-concurrency.ckpt`, and InnoDB tablespace
formats are unchanged.

## Verification Plan

- Build production primitive, embedded open/close, ownerless SQL, and
  performance probe targets.
- Run `libmylite.ownerless-primitives` to prove completed-range count updates
  on out-of-order record, merge, drain, and empty-drain paths.
- Run `libmylite.embedded-open-close` to prove the redo segment descriptor
  reports version `10`.
- Run a reduced stats-enabled ownerless bulk attribution probe to ensure redo
  and page-publication counters remain stable.
- Run a larger stats-off ownerless bulk probe to capture the production timing
  band.

## Verification Results

Passed:

- `cmake --build --preset embedded-prod --target
  mylite_ownerless_primitives_test mylite_embedded_open_close_test
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
- `ctest --preset embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-open-close)$'
  --output-on-failure`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-written-crash`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-latest-crash`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-latest-checkpoint-crash`
- `build/embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  redo-gap-blocks-writer`
- `ctest --preset embedded-prod -R
  'libmylite\.(ownerless-cross-process-sql\.(0|1|2))$'
  --output-on-failure`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_primitives_test
  mylite_embedded_open_close_test mylite_ownerless_cross_process_sql_test`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-open-close)$'
  --output-on-failure`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The final same-preset stats-enabled attribution sample used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It preserved the expected 100-row bulk write volume:

- `2.000` page-version records per statement;
- `4.500` page-log append calls per statement;
- `180.500` deferred latest-checkpoint coalesces per statement.

Compared with the prior local same-preset attribution sample, 100-row bulk
commit-log time moved from `1.348 ms` to `1.263 ms` per statement,
commit-log redo-leave time moved from `0.743 ms` to `0.687 ms` per statement,
and ownerless `mysql_query()` time moved from `4.455 ms` to `4.113 ms` per
statement.

The final stats-off 5000-row, 100-row-per-statement production sample reported:

- ordinary bulk: `88307.92 rows/s`;
- ownerless bulk: `24510.00 rows/s`;
- ownerless/ordinary bulk ratio: `0.2776`.

An earlier stats-off rerun in the same noisy local environment produced a lower
sample, so this slice records the subphase reduction and final stats-off band
as evidence for a bounded bookkeeping improvement rather than a broad
throughput guarantee.

## Acceptance Criteria

- Sequential contiguous redo completion skips completed-range slot scanning
  when the table is empty.
- Out-of-order range coalescing and draining keep the count accurate.
- Older `.shm` redo-state segment metadata is rejected through the version
  descriptor path.
- Focused tests and production timing probes pass.

## Risks And Non-Goals

- This does not address broader ownerless write costs in page publication,
  native redo/checkpoint reconciliation, SQL execution, DDL/file lifecycle, or
  external MariaDB/RQG stress.
- A stale or corrupt completed-range count would be unsafe, so the layout
  version is bumped and primitive tests exercise the count through the same
  public internal redo-state API used by hooks.
