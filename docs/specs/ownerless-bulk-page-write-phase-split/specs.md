# Ownerless Bulk Page-Write Phase Split

## Problem

The ownerless 100-row bulk insert path now has aggregate and deep InnoDB
first/remaining attribution, but the page-write, page-publish, page-log, and
commit-visibility counters were still reported only for the whole bulk loop.
That hides whether the later non-empty-table statement cost sits in redo leave,
MTR page publication, no-dirty page publication, page-log append, or commit
visibility.

Without a first/remaining split for those existing ownerless counters, the
next write-path optimization would still be driven by aggregate rows that mix
the first empty-table default-checked bulk statement with later row-level undo
statements.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/tests/embedded_performance_probe.c` already captures the
  first bulk statement timing and first deep InnoDB counter snapshot, then
  subtracts first counters from total counters for `first_` and `remaining_`
  deep rows.
- The same probe already has ownerless page-publish, database, page-write,
  page-log append, and commit-visibility counter readers. Those counters are
  reset before each bulk loop through `reset_ownerless_insert_stats()`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` owns the ownerless page-write and
  page-publish counters used by the probe. This slice does not edit those
  MariaDB-derived counters or their hot paths.

## Design

Add an optional `bulk_insert_first_stats` snapshot carrier to the embedded
performance probe. When stats-enabled bulk measurement completes the first
row-list statement, the probe snapshots the existing counters for:

- page publish,
- ownerless database hooks,
- page write,
- page-log append,
- commit visibility, and
- deep InnoDB counters.

The final ownerless bulk summary reads aggregate totals as before, then
subtracts the first snapshot from the totals to emit compact
`first_` and `remaining_` page-write phase rows. Existing aggregate metric
names remain unchanged.

The emitted phase rows cover page-version count, native-support publication
and elision, commit-visibility fast/flush counts, page-publish hook timing,
history-proof pair timing, page-log append timing, commit-log timing,
redo-leave subphases, no-dirty publish/leave/unlock subphases, and held
native-support page-write lock hits.

## Compatibility Impact

None. This is diagnostic-only instrumentation in the embedded performance
probe. It does not change SQL behavior, public C API behavior, storage format,
page-version WAL records, redo/checkpoint ordering, native InnoDB files,
ownerless lock ownership, or recovery.

## Directory And Lifecycle Impact

None. The probe still creates temporary MyLite directories and removes them at
the end of the run. No durable file, shared-memory field, or directory layout
changes.

## Native Storage Impact

None. Native row insert, undo, redo, mini-transaction commit, page publication,
and page-write release behavior are unchanged.

## Build And Performance Impact

Only first-party test/probe code changes. Stats-off production runs are
unchanged. Stats-enabled probe output grows by additional summary rows and the
first-statement snapshot performs a bounded set of counter-array reads after
the first bulk statement.

## Verification Plan

- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced two-statement stats-enabled 100-row bulk probe and verify new
  `first_` and `remaining_` page-write phase rows are emitted.
- Run a reduced one-statement stats-enabled 100-row bulk probe and verify
  `remaining_` page-write phase rows are zero.
- Run ownerless primitive coverage, production-build guard, format check, and
  `git diff --check`.

## Acceptance Criteria

- Existing aggregate ownerless bulk summary metric names remain present.
- Stats-enabled ownerless bulk output includes first and remaining phase rows
  for page-write/page-publish/page-log/commit-visibility counters.
- Remaining rows are computed by subtracting the first snapshot from total
  counters, not by reusing aggregate counters.
- One-statement bulk probes emit zero remaining page-write phase averages.
- The docs continue to describe this as attribution only, not a concurrency
  completion or non-empty-table undo-elision claim.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-primitives$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`
- Reduced two-statement stats-enabled probe:

  ```text
  MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=1
  MYLITE_PERF_INSERT_ITERATIONS=200
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
  ```

  The later non-empty-table statement emitted `2.000` page versions,
  `1.000` fast commit visibility events, `3.000` page-publish hook calls,
  `3.000` page-log append calls, `201.000` page-write commit-log calls,
  `0.438 ms` page-write commit-log time, `0.105 ms` redo-leave time,
  `0.178 ms` commit-log publish time, `0.244 ms` no-dirty loop time,
  `0.157 ms` no-dirty page-publish time, `0.010 ms` page-leave time, and
  `0.011 ms` page-unlock time per statement.

- Reduced one-statement guard:

  ```text
  MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=1
  MYLITE_PERF_INSERT_ITERATIONS=100
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
  ```

  The guard emitted `0.000` for remaining page versions, commit-visibility
  fast/flush counts, page-publish hook calls/time, page-log append calls/time,
  page-write commit-log calls/time, redo-leave time, commit-log publish time,
  no-dirty page-publish/page-leave/page-unlock time, and held native-support
  page-write lock hits.

## Risks And Follow-Up

- This exposes where the remaining ownerless 100-row bulk cost sits; it does
  not optimize that cost.
- The two-statement sample points the next bounded write-path work at
  non-empty-table commit-log/no-dirty-loop and page-publish cost, while native
  row-level undo, broader redo/checkpoint reconciliation, DDL/file-lifecycle
  recovery, and external stress remain separate ownerless completion work.
