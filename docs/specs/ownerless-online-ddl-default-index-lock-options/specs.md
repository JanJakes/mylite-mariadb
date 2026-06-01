# Ownerless Online DDL Default Index Lock Options

## Problem

Ownerless online DDL option coverage includes explicit no-lock and shared-lock
secondary-index variants plus instant column add/drop with `LOCK=DEFAULT`.
The remaining online DDL notes still call out broader option combinations. A
small accepted MariaDB shape is ordinary secondary-index add/drop with
`ALGORITHM=NOCOPY, LOCK=DEFAULT`, which should still publish a dictionary
generation and refresh already-open ownerless peers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:8164-8232` parses explicit `ALTER TABLE`
  `ALGORITHM` and `LOCK` clauses and stores the requested algorithm/lock mode.
- `mariadb/sql/sql_table.cc:8118-8220` resolves requested lock clauses,
  including `LOCK=DEFAULT`, before the in-place alter path runs.
- `mariadb/storage/innobase/handler/handler0alter.cc:2203-2351` documents
  InnoDB in-place alter support classes for ordinary index operations.
- `packages/libmylite/src/database.cc` wraps ownerless DDL in dictionary
  begin/finish calls and makes peers refresh local table metadata when they
  observe a newer stable dictionary generation.

## Scope And Non-Goals

In scope:

- Extend the `online-ddl-options` selector with
  `ALTER TABLE ... ADD INDEX ..., ALGORITHM=NOCOPY, LOCK=DEFAULT`.
- Extend the same selector with
  `ALTER TABLE ... DROP INDEX ..., ALGORITHM=NOCOPY, LOCK=DEFAULT`.
- Verify an already-open ownerless peer observes the added index, can use it
  through `FORCE INDEX`, and observes final index absence after drop.
- Keep final ownerless/native reopen checks before and after forced `.shm`
  rebuild.

Out of scope:

- Exhaust every MariaDB `ALGORITHM`/`LOCK` combination.
- Partitioned, full-text, spatial, and external randomized DDL oracles.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL semantics change. The slice strengthens ownerless `ALTER TABLE`
compatibility evidence for accepted online secondary-index option combinations
while keeping the matrix representative rather than exhaustive.

## Database Directory And Lifecycle Impact

No directory layout changes. Native metadata stays inside the MyLite database
directory and the final state is verified through ownerless and ordinary native
reopen before and after forced `.shm` rebuild.

## Native Storage Impact

Native InnoDB executes the index add/drop operations. MyLite must refresh peer
metadata across the ownerless dictionary generation boundary.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change adds focused SQL coverage and
documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `online-ddl-options` in `embedded-dev`.
- Run focused `online-ddl-options` in `ownerless-test-hooks`.
- Run adjacent broader DDL selector in embedded and hook builds if the focused
  selector passes.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- The already-open peer observes the `LOCK=DEFAULT` secondary index after add
  and can use it through `FORCE INDEX`.
- The peer observes the index absence after the `LOCK=DEFAULT` drop.
- Final rows, indexes, and metadata survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- MariaDB can reject some online option combinations for more complex table
  shapes; this slice uses a simple InnoDB table without full-text, spatial,
  partition, or virtual-column blockers.
- Broader online DDL option combinations and external randomized DDL oracles
  remain planned separately.
