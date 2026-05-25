# Prepared Insert Commit Component

## Problem

The prepared insert component benchmark times bind, step, and reset work inside
one transaction, but it did not time the final `COMMIT`. That made it possible
for append-buffer changes to appear faster by moving `pwrite()` cost from
`mylite_step()` into commit rather than reducing total storage work.

MyLite needs insert performance evidence that distinguishes per-row execution
overhead from deferred publication cost.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c::benchmark_prepared_insert_components()`
  creates an `ENGINE=InnoDB` table routed to MyLite, opens one transaction,
  executes all prepared inserts, finalizes the statement, commits, and then
  verifies row count.
- `packages/mylite-storage/src/storage.c::flush_statement_append_page_buffer()`
  writes buffered append pages either when the transient append buffer fills or
  before top-level header publication. A larger buffer can therefore shift
  measured work from `step` to final commit.

## Design

- Add a `prepared insert commit component` row to the existing
  `prepared-insert-components` phase.
- Time the final transaction commit after statement finalization.
- Keep bind, step, and reset timing unchanged.
- Expose the threshold metric name as `prepared-insert-commit`.

## Affected Subsystems

- Local performance baseline tooling.
- Row/index storage performance investigation.

## Compatibility Impact

No SQL, API, storage-engine routing, or file-format behavior changes. The
benchmark still uses `ENGINE=InnoDB` through the MyLite storage engine.

## Single-File And Lifecycle Impact

No lifecycle change. The new metric makes existing top-level publication cost
visible.

## Binary-Size Impact

Benchmark-only first-party change. No runtime library or dependency impact.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run:
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 50000`
- Run `git diff --check` and `git clang-format --diff`.

## Acceptance Criteria

- The prepared insert component phase prints bind, step, reset, and commit
  component rows.
- The commit metric can be used with `--max-us=prepared-insert-commit:<value>`.
- Existing prepared insert behavior and row-count verification remain
  unchanged.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `git diff --check`
- `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  reported:
  - `prepared insert step component`: `3.908 us/op`
  - `prepared insert commit component`: `287.937 ms`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 50000`
  reported:
  - `prepared insert step component`: `49.257 us/op`
  - `prepared insert commit component`: `891.859 ms`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --max-us=prepared-insert-commit:1000000 1000 1000`
  passed, validating the new threshold metric name.

## Risks And Unresolved Questions

- Commit is reported as one operation, not amortized per row. That keeps the
  total publication cost explicit; per-row amortized views can be added later
  if they help compare alternative storage designs.
