# Ownerless Online DDL Option Refresh

## Problem

Ownerless DDL coverage now proves many representative `ALTER TABLE` classes,
including ordinary index changes, column-shape changes, instant columns,
primary-key replacement, generated columns, CHECK constraints, table charset,
row format, comments, and forced rebuilds. The compatibility matrix still
leaves broader online DDL option variants planned. Applications commonly spell
MariaDB/MySQL DDL with explicit `ALGORITHM` and `LOCK` clauses, so ownerless
read/write mode needs bounded evidence that already-open peers refresh
dictionary and native InnoDB state after accepted option combinations.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:8164-8232` parses `ALTER TABLE` algorithm and lock
  options and stores them in `Alter_info`.
- `mariadb/sql/handler.h:100-110` defines the in-place ALTER result classes,
  including copy, no-copy, instant, shared-lock, and no-lock variants.
- `mariadb/storage/innobase/handler/handler0alter.cc:2200-2217` documents
  InnoDB's mapping from in-place ALTER support to `LOCK=NONE` and
  `LOCK=SHARED` paths.
- `handler0alter.cc:2456-2458` evaluates whether the operation can run online,
  and `handler0alter.cc:2847-2863` returns copy/no-copy lock classes for
  rebuild and ordinary index changes.
- `mariadb/sql/sql_table.cc:11589-11728` chooses the in-place path unless
  `ALGORITHM=COPY` was requested or the engine rejects the in-place form.
- `sql_table.cc:11760-11864` runs the copy ALTER path, including the lock
  upgrade and temporary table creation/copy sequence.
- `packages/libmylite/src/database.cc` already treats ownerless `ALTER TABLE`
  as dictionary DDL: it marks an active dictionary generation before dispatch,
  publishes a stable generation after success, and makes peers refresh table
  metadata before proceeding.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector for accepted explicit DDL option
  combinations:
  - `ALGORITHM=NOCOPY, LOCK=NONE` for ordinary secondary-index creation,
  - `ALGORITHM=INPLACE, LOCK=SHARED` for another secondary-index creation,
  - `ALGORITHM=COPY, LOCK=EXCLUSIVE` for a column definition copy rebuild,
  - `ALTER TABLE ... FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE` for an explicit
    table rebuild.
- Verify an already-open ownerless peer observes index metadata, column metadata,
  row contents, and DML usability after each optioned DDL boundary.
- Verify the final state through ownerless and ordinary exclusive reopen before
  and after forced `.shm` rebuild.

Out of scope:

- Unsupported partition, special-index, `DISCARD/IMPORT TABLESPACE`,
  stored-routine, sequence, SQL locked-table, and table-admin surfaces.
- Claiming every `ALGORITHM`/`LOCK` combination works; MariaDB rejects some
  combinations by design, and this slice only covers accepted representative
  combinations.
- SQL-level table-lock fault injection. Prior investigation found explored SQL
  shapes are intercepted before the ownerless table-wait callback.
- External MariaDB/RQG randomized DDL oracle stress.

## Design

Add `online-ddl-options` to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

A child ownerless process creates a base InnoDB table, then waits for the parent
between DDL steps:

1. Add a secondary index with `ALGORITHM=NOCOPY, LOCK=NONE`.
2. Add another secondary index with `ALGORITHM=INPLACE, LOCK=SHARED`.
3. Widen a `VARCHAR` column with `ALGORITHM=COPY, LOCK=EXCLUSIVE`.
4. Run `ALTER TABLE ... FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE`.

The parent keeps an ownerless handle open for the whole sequence. After each
stable dictionary generation, it checks `information_schema` metadata, uses
`FORCE INDEX` where relevant, performs DML through the refreshed definition, and
checks row totals. Final helper assertions reopen the database in ownerless and
ordinary exclusive modes before and after deleting volatile `.shm`.

## Compatibility Impact

No SQL semantics or public API change. This is compatibility evidence for
explicit MariaDB `ALTER TABLE` option clauses in ownerless read/write mode. The
compatibility matrix should name the covered option variants and continue to
mark unsupported DDL classes and randomized oracle stress as planned.

## Directory And Lifecycle Impact

The slice adds no durable files and does not change directory layout. It
exercises MariaDB native `.frm` and InnoDB tablespace updates under the existing
MyLite database directory, plus the existing ownerless dictionary generation in
`concurrency/mylite-concurrency.shm`.

## Native Storage Impact

The test uses MariaDB/InnoDB native ALTER TABLE execution. MyLite does not
emulate index or table-rebuild state; it verifies that the ownerless lifecycle
refreshes peers around MariaDB's accepted DDL option paths.

## Binary Size And Dependencies

No default binary profile, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `online-ddl-options` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL CTest.
- Run the hook ownerless cross-process SQL CTest or the relevant hook selector
  subset if known intermittent InnoDB log-header checksum aborts appear.
- Run ownerless stress if the focused and full ownerless SQL checks are green.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- An already-open ownerless peer sees and can use indexes added through explicit
  `NOCOPY`/`NONE` and `INPLACE`/`SHARED` option clauses.
- The peer sees the widened column after an explicit `COPY`/`EXCLUSIVE` alter
  and can write values that require the new definition.
- The final table state survives an explicit copy `FORCE` rebuild.
- Ownerless and ordinary exclusive reopen, including forced `.shm` rebuild,
  observe the same rows, indexes, and column metadata.
- Docs distinguish the bounded option coverage from unsupported DDL surfaces
  and external randomized DDL stress.

## Risks And Follow-Up

- This does not prove every legal MariaDB algorithm/lock combination or every
  table shape. It targets deterministic accepted combinations over a simple
  InnoDB table.
- Broader DDL/file-lifecycle recovery metadata and external MariaDB/RQG stress
  remain separate ownerless-concurrency work.
