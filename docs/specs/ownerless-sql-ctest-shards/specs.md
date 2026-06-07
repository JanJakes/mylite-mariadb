# Ownerless SQL CTest Shards

## Problem

The ownerless cross-process SQL aggregate has grown large enough that broad
registered CTest shards can hit their 900-second timeout under ordinary CI or
shared-runner load. When a shard or internal child case times out without
progress output, CTest reports only the shard name, hiding the selector or case
that was running and making failures hard to triage.

## Design

Keep the no-argument `mylite_ownerless_cross_process_sql_test` aggregate for
manual full-suite runs, and add a public `sql-shard <index> <count>` command
that runs internal ownerless SQL cases whose case index belongs to that shard.
The shard command reuses the existing internal per-case fork dispatcher, so new
ownerless SQL cases are covered automatically without duplicating the selector
list in CMake.

Register eight CTest shards through the current weighted shard command:

```sh
mylite_ownerless_cross_process_sql_test sql-weighted-shard 0 8
mylite_ownerless_cross_process_sql_test sql-weighted-shard 1 8
mylite_ownerless_cross_process_sql_test sql-weighted-shard 2 8
mylite_ownerless_cross_process_sql_test sql-weighted-shard 3 8
mylite_ownerless_cross_process_sql_test sql-weighted-shard 4 8
mylite_ownerless_cross_process_sql_test sql-weighted-shard 5 8
mylite_ownerless_cross_process_sql_test sql-weighted-shard 6 8
mylite_ownerless_cross_process_sql_test sql-weighted-shard 7 8
```

The original modulo `sql-shard <index> <count>` command remains available for
manual comparison and bisecting older logs, but normal CTest registration uses
`sql-weighted-shard` so predicted heavier cases are spread across shards.

All shards retain the existing `compat.ownerless-cross-process-sql` label and
900-second per-test timeout, so existing label-based commands continue to run
the full ownerless SQL suite while producing per-shard timing and failure
identity. The shard command prints flushed `ownerless-sql case start` and
`ownerless-sql case pass` diagnostics with the internal case index and elapsed
seconds, so a timeout's captured output identifies the last active case. The
per-case child dispatcher also has a 300-second watchdog. Weighted shard start
diagnostics include the estimated shard weight so imbalance is visible in CTest
logs. If an internal case child does not exit before the watchdog, the parent prints
`ownerless-sql case timeout` with the case index, PID, and timeout seconds,
kills the child, and fails immediately instead of spending the remaining outer
CTest shard budget on one stuck case.

## Compatibility Impact

No product behavior changes. This only changes how the ownerless SQL regression
suite is registered with CTest.

## Test Plan

- Configure/build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run representative direct internal cases and a verbose shard to confirm the
  active-case diagnostics still print.
- Run focused storage-option policy coverage.
- Run `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql`.
- Run the same label under `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, and diff checks.

## Acceptance Criteria

- Existing focused selector arguments continue to work.
- `ctest -L compat.ownerless-cross-process-sql` discovers eight normal
  ownerless SQL weighted-shard tests in embedded builds.
- Each shard can fail independently with its own CTest name.
- The full label no longer depends on one monolithic or oversized 900-second
  test budget.
- Timeout output identifies the active ownerless SQL internal case index.
- A hung internal case fails through the case watchdog with case index and PID
  diagnostics before the 900-second outer CTest timeout.
