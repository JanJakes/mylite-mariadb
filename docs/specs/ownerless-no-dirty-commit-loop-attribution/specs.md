# Ownerless No-Dirty Commit-Loop Attribution

## Problem

The production ownerless write-path probe showed that read and reconnect paths
are close to ordinary MariaDB behavior, while autocommit and small bulk insert
throughput still lag. The remaining `page_write_commit_log_no_dirty_loop_ms`
counter was too broad: it included native page unlocks, ownerless page-write
leave checks, page publication for no-dirty mini-transactions, and ownerless
space-write leave work.

That made the next write-path optimization ambiguous and risked repeating
unsafe MTR release shortcuts that failed stress coverage earlier.

## Scope

In scope:

- Split the no-dirty ownerless MTR commit-log loop into sub-counters for:
  - space-write leave,
  - page publication,
  - page-write leave,
  - native page unlock.
- Emit the new counters through the production embedded performance probe.
- Add bulk insert summary rows so CI can distinguish per-row and per-statement
  no-dirty loop cost.

Out of scope:

- Changing MTR release order, page-write ownership, page publication, redo,
  checkpoint, WAL format, or SQL-visible behavior.
- Optimizing exact-delta page-log fallback or native-support proof publication
  in this slice.

## Design

The existing page-write perf-stat array remains the single ABI between the
InnoDB hook code and the first-party performance probe. Four counters are
appended after the existing no-dirty loop aggregate so existing earlier index
values remain stable:

- `commit_log_no_dirty_space_leave_ns`
- `commit_log_no_dirty_page_publish_ns`
- `commit_log_no_dirty_page_leave_ns`
- `commit_log_no_dirty_page_unlock_ns`

The timers are active only when ownerless page-write perf stats are enabled.
The disabled production path still sees the existing zero-start fast return in
the elapsed-time helper.

## Compatibility Impact

No SQL, C API, PHP API, storage-format, WAL-format, or directory-layout change.
The only user-visible difference is additional diagnostic output from
`mylite_embedded_performance_probe`.

## Test Plan

- Rebuild `mylite_embedded_performance_probe` under the production embedded
  preset.
- Run a reduced production stats-enabled performance probe and verify the new
  raw and summary fields are emitted.
- Run focused ownerless SQL coverage that already proves disabled page-write
  stats stay zero.
- Run formatting and diff checks.

## Acceptance Criteria

- The new counters are appended without renumbering existing perf-stat indexes.
- The production probe emits raw no-dirty sub-counters for transaction,
  autocommit, and bulk insert phases.
- The production probe emits compact `mylite_perf_summary_*` rows for
  autocommit and bulk no-dirty sub-counters.
- Focused ownerless coverage remains green.

## Verification

- `tools/mariadb-embedded-build build` confirmed the MinSizeRel embedded
  MariaDB archive was fresh.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- Three reduced production stats-enabled probes with
  `MYLITE_PERF_INSERT_ITERATIONS=120` emitted the new raw and summary fields.
  The bulk insert samples reported
  `page_write_commit_log_no_dirty_loop_ms_per_statement=0.049` to `0.070`,
  split into `page_publish_ms_per_statement=0.039` to `0.055`,
  `page_leave_ms_per_statement=0.008` to `0.013`,
  `page_unlock_ms_per_statement=0.000`, and
  `space_leave_ms_per_statement=0.000`.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` passed 16/16.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Risks And Follow-Up

This slice intentionally measures before optimizing. The next write-path slice
should target no-dirty page publication before page-write leave or native page
unlock on this probe shape.
