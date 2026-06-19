# Ownerless FK Third-Row Crash

## Problem Statement

Ownerless FK referential-action crash coverage already proves writer death
before child action execution, after child action return, before a child
`row_upd()` call, and after the first and second matching child-side
`row_upd()` calls. The remaining bounded gap is whether the same recovery
invariant holds after a later matching child row in the same referential action
has already been modified.

This slice adds deterministic third matching child-row crash coverage for
ordinary and generated-column FK action paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_update_cascade_for_mysql()` drives FK cascade execution by repeatedly
  calling `row_upd_step(thr)`.
- `mariadb/storage/innobase/row/row0upd.cc` fires the unsafe MyLite
  `foreign-key-action-row-step-after-update` test fault after a successful
  child-side `row_upd()` while `thr->fk_cascade_depth > 0`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  supports `MYLITE_OWNERLESS_TEST_FAULT_SKIP`, allowing a deterministic fault
  after the third matching hook occurrence by skipping the first two matches.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already owns
  the FK action crash fixtures, live-peer crash helper, no-live recovery
  assertions, and forced `.shm` rebuild checks.

## Design

Add isolated third-row fixtures instead of changing the existing two-row
fixtures:

- ordinary FK action crash tests seed three matching `ON UPDATE CASCADE` and
  three matching `ON DELETE CASCADE` / `ON DELETE SET NULL` rows for each
  affected parent;
- generated-column FK action crash tests seed three matching generated child
  rows and three matching referenced-column child rows;
- new direct selectors use `MYLITE_OWNERLESS_TEST_FAULT_SKIP=2` through
  `execute_sql_until_ownerless_fault_after_skips()`.

The selectors are:

- `foreign-key-action-row-step-third-after-crash`;
- `generated-column-foreign-key-action-row-step-third-after-crash`.

The ordinary FK final-state assertion is separate because the extra matching
child rows intentionally change surviving child totals. The generated-column
final-state assertion remains unchanged because the extra rows are deleted by
the retried parent deletes before the final reopen checks.

## Compatibility Impact

No supported SQL behavior changes. This is hook-only crash coverage under
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS`. The compatibility claim improves
from deterministic first/second-row FK row-step crash evidence to
first/second/third-row evidence.

## Directory And Lifecycle Impact

No durable file or directory-layout changes. The tests keep using temporary
MyLite-owned database directories, live-peer cleanup checks, no-live recovery,
ownerless/native reopen, and forced `.shm` rebuild checks.

## Native Storage Impact

MyLite still relies on MariaDB/InnoDB rollback and recovery semantics for the
interrupted FK statement. The slice proves ownerless recovery does not expose
partially applied third child-row referential-action state after writer death.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `foreign-key-action-row-step-third-after-crash`;
  - `generated-column-foreign-key-action-row-step-third-after-crash`.
- Run adjacent second-row selectors and `foreign-key-actions`.
- Run the hook crash-tail aggregate or the focused ownerless hook subset.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Both new third-row selectors pass in the unsafe hook build.
- Existing first/second-row FK crash selectors keep their original two-row
  fixture and final-state oracles.
- Docs identify broader randomized FK graph crash fuzzing and external
  MariaDB/RQG stress as still planned.

## Risks And Unresolved Questions

- This is deterministic third-row evidence, not exhaustive FK row-order fuzzing
  across arbitrary graph shapes.
- SQL-level table-lock fault injection is unrelated and remains unproven for
  representative SQL shapes that do not reach the local ownerless table-wait
  callback.
