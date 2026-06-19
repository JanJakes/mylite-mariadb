# Ownerless FK Sixth-Row Crash

## Problem Statement

Ownerless FK referential-action crash coverage now reaches the first, second,
and third matching child-side `row_upd()` callbacks. That still leaves a bounded
later-row gap: recovery must not depend on the first few rows of a cascade being
special.

This slice adds deterministic sixth matching child-row crash coverage for the
ordinary and generated-column FK action paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` drives FK cascade execution through
  repeated `row_upd_step(thr)` calls.
- `mariadb/storage/innobase/row/row0upd.cc` fires the unsafe MyLite
  `foreign-key-action-row-step-after-update` test fault after a successful
  child-side `row_upd()` while `thr->fk_cascade_depth > 0`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  supports `MYLITE_OWNERLESS_TEST_FAULT_SKIP`, so the hook can skip the first
  five matching occurrences and kill the writer after the sixth one.

## Design

Parameterize the existing hook-only FK action crash fixtures by matching child
rows per affected parent:

- existing pre-action, post-action, row-step-before, row-step-after, and
  second-row selectors keep the two-row fixture;
- the third-row selectors use a three-row fixture;
- new sixth-row selectors use six matching child rows and
  `MYLITE_OWNERLESS_TEST_FAULT_SKIP=5`.

The selectors are:

- `foreign-key-action-row-step-sixth-after-crash`;
- `generated-column-foreign-key-action-row-step-sixth-after-crash`.

The ordinary FK assertion computes exact final CASCADE and SET NULL totals from
the fixture row count. Generated-column final-state checks remain unchanged
because the retried deletes remove all extra matching rows before the final
reopen checks.

## Compatibility Impact

No supported SQL behavior changes. This is unsafe hook-only crash coverage under
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS`. The compatibility claim improves
from deterministic first/second/third-row FK row-step crash evidence to
first/second/third/sixth-row evidence.

## Directory And Lifecycle Impact

No durable file or directory-layout changes. The tests continue to use
temporary MyLite-owned database directories, live-peer cleanup checks, no-live
recovery, ownerless/native reopen, and forced `.shm` rebuild checks.

## Native Storage Impact

MyLite still relies on MariaDB/InnoDB rollback and recovery semantics for the
interrupted FK statement. This slice proves ownerless recovery does not expose a
partially applied sixth child-row referential-action state after writer death.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `foreign-key-action-row-step-sixth-after-crash`;
  - `generated-column-foreign-key-action-row-step-sixth-after-crash`.
- Run adjacent third-row selectors and `foreign-key-actions`.
- Run the hook crash-tail aggregate.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Both new sixth-row selectors pass in the unsafe hook build.
- Existing two-row and third-row selectors keep their intended row-count
  fixtures and exact final-state oracles.
- Docs identify randomized FK graph crash fuzzing and external MariaDB/RQG
  stress as still planned.

## Risks And Unresolved Questions

- This is deterministic sixth-row evidence, not exhaustive FK row-order fuzzing
  across arbitrary graph shapes.
- SQL-level table-lock fault injection is unrelated and remains unproven for
  representative SQL shapes that do not reach the local ownerless table-wait
  callback.
