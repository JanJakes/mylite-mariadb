# Ownerless SQL CTest Shards

## Problem

The ownerless cross-process SQL aggregate has grown large enough that the
single registered CTest can hit its 900-second timeout under ordinary CI or
shared-runner load. When it times out, CTest reports only
`libmylite.ownerless-cross-process-sql`, hiding the selector or case that was
running and making failures hard to triage.

## Design

Keep the no-argument `mylite_ownerless_cross_process_sql_test` aggregate for
manual full-suite runs, and add a public `sql-shard <index> <count>` command
that runs internal ownerless SQL cases whose case index belongs to that shard.
The shard command reuses the existing internal per-case fork dispatcher, so new
ownerless SQL cases are covered automatically without duplicating the selector
list in CMake.

Register four CTest shards:

```sh
mylite_ownerless_cross_process_sql_test sql-shard 0 4
mylite_ownerless_cross_process_sql_test sql-shard 1 4
mylite_ownerless_cross_process_sql_test sql-shard 2 4
mylite_ownerless_cross_process_sql_test sql-shard 3 4
```

All shards retain the existing `compat.ownerless-cross-process-sql` label and
900-second per-test timeout, so existing label-based commands continue to run
the full ownerless SQL suite while producing per-shard timing and failure
identity.

## Compatibility Impact

No product behavior changes. This only changes how the ownerless SQL regression
suite is registered with CTest.

## Test Plan

- Configure/build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused storage-option policy coverage.
- Run `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql`.
- Run the same label under `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, and diff checks.

## Acceptance Criteria

- Existing focused selector arguments continue to work.
- `ctest -L compat.ownerless-cross-process-sql` discovers four normal
  ownerless SQL shard tests in embedded builds.
- Each shard can fail independently with its own CTest name.
- The full label no longer depends on one monolithic 900-second test budget.
