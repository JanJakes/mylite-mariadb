# Ownerless FK Later-Row Crash

## Problem Statement

Ownerless foreign-key referential-action crash coverage already kills writers
before a child action executes, after a child action returns, before
`row_upd()` applies a child row, and after the first child-side `row_upd()`
succeeds in a multi-child cascade. The remaining documented gap is later child
row crash selection: a parent action can modify more than one matching child row
before the parent statement commits, so recovery evidence should include a
later intra-action child-row boundary instead of only the first row.

This slice adds deterministic hook-build coverage for the second matching
child-side `row_upd()` in ordinary and generated-column foreign-key action
crash tests.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), per
  `docs/architecture/engineering-standards.md`.
- `mariadb/storage/innobase/row/row0ins.cc` prepares the cascade node for
  referential actions, sets `cascade->state = UPD_NODE_UPDATE_CLUSTERED`, and
  calls `row_update_cascade_for_mysql()`. Existing MyLite unsafe test hooks fire
  at `foreign-key-action-before-execute` and
  `foreign-key-action-after-execute`.
- `mariadb/storage/innobase/row/row0mysql.cc` implements
  `row_update_cascade_for_mysql()`, repeatedly invoking `row_upd_step(thr)`
  for the child-table action under `thr->fk_cascade_depth > 0`.
- `mariadb/storage/innobase/row/row0upd.cc` implements `row_upd_step()` and
  already fires `foreign-key-action-row-step-before-update` before
  `row_upd()` and `foreign-key-action-row-step-after-update` after a successful
  child-row update/delete when inside a foreign-key cascade.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements the unsafe hook fault dispatcher by matching
  `MYLITE_OWNERLESS_TEST_FAULT` and then blocking after signaling the ready fd.
  The dispatcher currently has no way to skip the first matching occurrence.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already seeds
  multi-child ordinary FK action rows and generated-column FK action rows, then
  verifies no-live recovery rolls back uncommitted parent/child state before
  retry.

## Design

Add a test-only `MYLITE_OWNERLESS_TEST_FAULT_SKIP` environment variable to the
unsafe InnoDB fault dispatcher. It is interpreted as the number of matching
fault occurrences to ignore before the normal ready-fd/blocking fault behavior.
The default remains `0`, preserving current tests and product behavior.

Use that skip in new ownerless SQL hook selectors that kill ordinary and
generated-column FK action writers on the second
`foreign-key-action-row-step-after-update` event. The existing multi-child
tables are intentionally reused so the test remains a bounded extension of the
current recovery oracle instead of a new randomized matrix.

## Affected Subsystems

- MariaDB-derived InnoDB unsafe MyLite test hooks.
- Ownerless cross-process SQL hook tests.
- Ownerless compatibility and cross-process concurrency documentation.

## Compatibility Impact

No supported SQL behavior changes. The new skip behavior is inactive unless
unsafe ownerless test hooks enable InnoDB test faults and a matching
`MYLITE_OWNERLESS_TEST_FAULT` is configured. The compatibility claim changes
from first-row-only foreign-key partial-action crash evidence to deterministic
first-row and second-row evidence, while broader randomized later-row FK crash
fuzzing and full RQG-style graph stress remain planned.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The tests continue to use a
temporary MyLite database directory, crash a child ownerless writer while a live
peer exists, verify live-peer cleanup stays busy, then prove no-live recovery
and forced `.shm` rebuild visibility.

## Native Storage Impact

The covered native InnoDB path is still MariaDB's referential action execution
inside `row_update_cascade_for_mysql()` and `row_upd_step()`. MyLite does not
replace InnoDB's rollback semantics; the slice adds proof that ownerless
recovery does not expose partially applied second child-row action state after
writer death.

## Public API, Wire Protocol, Size, License, And Dependencies

No public C API, wire-protocol, dependency, license, or meaningful production
binary-size change is intended. Production builds keep InnoDB test faults
disabled, so the new environment variable is not consulted on ordinary
ownerless runtime paths.

## Test And Verification Plan

- Build the unsafe ownerless hook target that includes the InnoDB test fault
  dispatcher and ownerless SQL test binary.
- Run direct selectors:
  - `foreign-key-action-row-step-second-after-crash`
  - `generated-column-foreign-key-action-row-step-second-after-crash`
- Run the focused hook aggregate or hook ownerless CTest subset that includes
  the existing and new FK row-step crash tests.
- Run `ctest --preset prod -R tools.check-ci-production-builds`.
- Run `git diff --check` and format verification.

## Acceptance Criteria

- Existing first-row row-step-after FK crash tests keep passing.
- New second-row row-step-after FK crash tests pass for ordinary and
  generated-column FK actions.
- Docs distinguish deterministic second-row evidence from still-planned
  randomized later-row crash fuzzing.
- No product ownerless runtime path depends on the new skip environment
  variable.

## Risks And Unresolved Questions

- The new deterministic skip proves a later child-row boundary, not exhaustive
  row-order coverage for every FK graph shape.
- MariaDB can choose child-table and row iteration order; the tests assert
  recovery invariants, not a user-visible order.
- Long-running external MariaDB/RQG FK graph execution remains planned.
