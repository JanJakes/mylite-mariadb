# Ownerless Inplace Lock-Default DDL

## Problem

Ownerless online DDL coverage already verifies representative explicit
`NOCOPY`, `INPLACE`, `INSTANT`, and `COPY` forms, plus recent `LOCK=DEFAULT`
instant column and `NOCOPY` secondary-index forms. A remaining bounded gap is
ordinary InnoDB secondary-index DDL using `ALGORITHM=INPLACE,
LOCK=DEFAULT`, where MariaDB chooses the effective metadata-lock behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `ha_innobase::check_if_supported_inplace_alter()` returns
  `HA_ALTER_INPLACE_NOCOPY_NO_LOCK` or
  `HA_ALTER_INPLACE_NOCOPY_LOCK` for ordinary no-rebuild secondary-index
  add/drop paths after checking online safety.
- `mariadb/sql/sql_table.cc` `mysql_inplace_alter_table()` upgrades to
  `MDL_EXCLUSIVE` for prepare, then downgrades according to the storage-engine
  result and the requested lock. `LOCK=DEFAULT` therefore exercises MariaDB's
  default lock selection instead of an explicitly requested `LOCK=NONE` or
  `LOCK=SHARED`.
- `mariadb/sql/handler.h` documents the in-place alter result classes and their
  lock implications for copy/no-copy and online/offline execution.

## Design

Extend the existing `online-ddl-options` ownerless SQL selector with a peer
refresh step for:

- `ALTER TABLE ... ADD INDEX ..., ALGORITHM=INPLACE, LOCK=DEFAULT`
- `ALTER TABLE ... DROP INDEX ..., ALGORITHM=INPLACE, LOCK=DEFAULT`

The child process performs each DDL statement while the parent keeps an
already-open ownerless peer attached. After each step, the parent verifies
`information_schema.statistics` metadata and actual index usability through
`FORCE INDEX`, then verifies the dropped-index form fails through the same
already-open handle. Final ownerless and native exclusive reopens, including
forced `.shm` rebuild, verify durable metadata state.

## Scope

In scope:

- Ordinary InnoDB secondary indexes on a non-partitioned table.
- Explicit `ALGORITHM=INPLACE, LOCK=DEFAULT` add/drop forms.
- Ownerless peer dictionary refresh and final no-live/native reopen checks.

Out of scope:

- `FULLTEXT`, `SPATIAL`, partitioned, foreign-key, primary-key, generated
  column, and tablespace detach/import DDL.
- SQL-level table-lock fault injection. Prior negative-proof coverage shows
  representative SQL shapes time out before the ownerless table-wait callback.
- External randomized MariaDB/RQG execution.

## Compatibility Impact

This slice narrows the ownerless `ALTER TABLE` partial-support gap for an
ordinary MariaDB InnoDB online DDL option combination. It does not change SQL
semantics or public API behavior. Unsupported online DDL classes remain
explicitly planned or rejected by existing policy coverage.

## Directory And Lifecycle Impact

No durable layout changes are introduced. The test verifies the final metadata
and engine state through ownerless reopen, ordinary native exclusive reopen,
and reopen after removing `concurrency/mylite-concurrency.shm`.

## Native Storage Impact

The slice relies on MariaDB/InnoDB native in-place secondary-index DDL. It adds
no new ownerless storage format or recovery path.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run `mylite_ownerless_cross_process_sql_test online-ddl-options`.
- Build and run the same selector in `ownerless-test-hooks`.
- Run the selector from the embedded build directory to match CTest's working
  directory.
- Run the no-argument embedded ownerless SQL binary as a broader regression
  gate; if the CTest wrapper hits a known native log-scan abort, isolate and
  rerun the failing selector.
- Run `format-check` and `git diff --check`.
- Confirm no `/tmp/mylite-ownerless-*` directories or ownerless test processes
  remain.

## Acceptance Criteria

- Already-open ownerless peers observe the added `INPLACE, LOCK=DEFAULT`
  secondary index and can use it.
- Already-open ownerless peers observe the dropped `INPLACE, LOCK=DEFAULT`
  secondary index and cannot use it.
- Final state is readable through ownerless and ordinary native exclusive
  reopen before and after forced `.shm` rebuild.
- Compatibility docs list the newly covered option combination without
  broadening unsupported DDL claims.

## Risks

- This is an option-matrix coverage slice, not a broader DDL recovery slice.
  Native redo/checkpoint reconciliation and durable file-lifecycle metadata
  still gate any full ownerless concurrency completion claim.
