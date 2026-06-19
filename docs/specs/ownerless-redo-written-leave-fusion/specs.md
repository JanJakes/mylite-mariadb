# Ownerless Redo Written Leave Fusion

## Problem

The production ownerless bulk attribution probe with 5000 rows and 100 rows
per statement showed the remaining gap mostly inside native execution. The
largest ownerless write-path buckets included row insert/undo work and repeated
ownerless redo/page-write hook overhead:

- ownerless bulk throughput was `18594.73 rows/s` versus ordinary bulk
  `108178.15 rows/s`;
- ownerless bulk `mysql_query()` time was `4.895 ms` per statement versus
  ordinary `0.832 ms`;
- ownerless page-write commit-log work was `1.674 ms` per statement;
- the commit-log redo-leave subphase alone was `0.988 ms` per statement;
- the same sample reported `9855` redo written/leave calls for `50` bulk
  statements.

The redo written and redo leave callbacks both update the same shared redo
state segment. In the production top-level mini-transaction path, the written
range and final latest LSN are known together, but the implementation acquired
the shared progress latch once for `complete_write()` and again for `leave()`.

## Design

Add an optional MyLite-owned fused InnoDB hook:

- `mylite_ownerless_innodb_redo_written_and_leave()` preserves the existing
  `mylite_ownerless_innodb_redo_written()` plus
  `mylite_ownerless_innodb_redo_leave()` fallback;
- the fused path is used only for top-level redo depth with an installed
  combined callback;
- unsafe ownerless hook builds stay on the separate written/leave callbacks so
  existing named crash windows remain individually observable;
- range-less leave paths continue to call the existing leave hook directly.

The shared redo-state primitive
`mylite_ownerless_redo_state_complete_write_and_leave()` reuses the existing
write-completion and active-owner leave logic under one progress-latch
acquisition. It does not change redo reservation ordering, active reservation
cleanup, completed-range draining, latest-LSN publication, checkpoint
persistence, or page-version WAL format.

## Compatibility Impact

No SQL, C API, WAL format, native file format, directory layout, or public
ownerless behavior changes. This is an ownerless production hot-path
optimization and an internal hook extension. Existing separate callbacks
remain available and remain the conservative fallback.

The fused hook increments the existing redo written and redo leave call
counters. Its combined elapsed time is attributed to the redo leave bucket;
MTR-level performance counters continue to measure the full ownerless
commit-log redo-leave interval.

## Verification Plan

- Build focused production targets for ownerless primitives, embedded InnoDB
  hook tests, ownerless SQL, and the embedded performance probe.
- Run the primitive and embedded hook tests to prove the fused primitive and
  fallback dispatch.
- Run the focused ownerless multi-row visible-fast selector.
- Run a reduced production stats-enabled bulk attribution probe with 5000 rows
  and 100 rows per statement, then compare redo-leave and throughput counters
  against the pre-slice baseline above.
- Run unsafe hook crash selectors that depend on separate redo fault windows.
- Run format and whitespace checks.

## Acceptance Criteria

- Separate `complete_write()` plus `leave()` and fused
  `complete_write_and_leave()` produce matching redo-state snapshots in the
  primitive test.
- The hook test proves top-level fused dispatch and fallback separate dispatch.
- Focused ownerless SQL remains correct.
- The reduced production attribution probe shows the fused path active without
  increasing page-version, page-log, or native-support publication volume.
- The change is recorded as a bounded performance slice, not as full ownerless
  concurrency completion.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_primitives_test
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test
  mylite_embedded_performance_probe`
- `ctest --preset embedded-prod -R
  'libmylite\.(ownerless-primitives|embedded-ownerless-innodb-lock-hooks|ownerless-single-owner-multi-row-insert-visible-fast-path)$'
  --output-on-failure`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(ownerless-primitives|embedded-ownerless-innodb-lock-hooks|ownerless-native-table-wait|ownerless-native-table-wait-crash|ownerless-stale-drop-crash-recovery|ownerless-history-proof-publish-failure-fallback)$'
  --output-on-failure`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-stress$' --output-on-failure`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced stats-enabled production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=5000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

Compared with the pre-slice local baseline, the post-slice sample reported:

- ownerless bulk rows: `19629.59 rows/s` versus `18594.73 rows/s` before;
- ownerless/ordinary bulk row ratio: `0.2052` versus `0.1719` before;
- ownerless bulk `mysql_query()`: `4.614 ms` per statement versus `4.895 ms`
  before;
- ownerless page-write commit-log: `1.382 ms` per statement versus
  `1.674 ms` before;
- ownerless commit-log redo-leave: `0.737 ms` per statement versus
  `0.988 ms` before.

Publication volume stayed on the same bounded path:

- `2.000` page versions per statement;
- `4.380` page-log append calls per statement;
- `2.000` native-support published pages per statement;
- `1.000` visible-fast commit per statement.
