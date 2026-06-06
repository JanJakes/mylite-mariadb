# Ownerless INPLACE Lock-Shared Drop DDL

## Problem Statement

Ownerless online DDL option coverage verifies representative `NOCOPY`,
`INPLACE`, `INSTANT`, and `COPY` paths, but the matrix still marks broader
accepted option combinations as planned. The existing `online-ddl-options`
selector creates a secondary index with `ALGORITHM=INPLACE, LOCK=SHARED` and
later removes it through a different `NOCOPY, LOCK=NONE` shape. That does not
prove that an already-open ownerless peer observes the standalone
`INPLACE, LOCK=SHARED` drop boundary, nor that the same peer refreshes after a
re-add through the same option combination.

Non-goals:

- exhaust every MariaDB `ALGORITHM`/`LOCK` combination,
- enable unsupported `FULLTEXT`, `SPATIAL`, partition, table-directory, or
  tablespace detach/import DDL in ownerless mode,
- prove SQL-level table-lock fault injection for native table-wait paths, and
- replace external randomized MariaDB/RQG DDL oracle follow-up work.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_alter.cc:70-98` parses explicit `ALTER TABLE`
  `ALGORITHM` and `LOCK` clauses into `Alter_info`.
- `mariadb/sql/sql_alter.cc:138-218` validates requested algorithm and lock
  clauses against the handler's reported in-place capability.
- `mariadb/sql/sql_table.cc:11589-11713` routes non-copy ALTER operations
  through the in-place path, asks the handler for support, and enforces the
  requested algorithm/lock combination.
- `mariadb/mysql-test/main/alter_table.test` covers ordinary `ADD INDEX` with
  `ALGORITHM=INPLACE` and `LOCK=NONE`/`SHARED`/`EXCLUSIVE`, and InnoDB tests use
  `ALGORITHM=INPLACE, LOCK=NONE` for combined add/drop-index shapes.
- `packages/libmylite/src/database.cc` wraps ownerless DDL in the dictionary
  begin/finish boundary and refreshes peers when they observe a newer stable
  dictionary generation.

## Design

Extend the existing `online-ddl-options` ownerless SQL selector. The selector
already keeps one ownerless DDL process and one already-open ownerless peer
synchronized at every DDL boundary, then verifies final state through
ownerless reopen, ordinary native exclusive reopen, forced `.shm` rebuild, and
native reopen after rebuild.

After the existing `ALGORITHM=INPLACE, LOCK=SHARED` secondary-index add, add two
boundaries:

```sql
ALTER TABLE app.ownerless_ddl_options
  DROP INDEX ownerless_ddl_options_value_idx,
  ALGORITHM=INPLACE, LOCK=SHARED;

ALTER TABLE app.ownerless_ddl_options
  ADD INDEX ownerless_ddl_options_value_idx (value),
  ALGORITHM=INPLACE, LOCK=SHARED;
```

The already-open peer verifies the index is unusable after the drop and usable
again after the re-add. A later existing `NOCOPY, LOCK=NONE` drop still proves
the final state has no stale `value_idx` metadata.

## Compatibility Impact

No SQL semantics change. The slice strengthens evidence that ownerless
read/write peers observe MariaDB-supported in-place/shared-lock secondary-index
DDL boundaries for both addition and removal. Unsupported DDL classes and full
randomized DDL oracle execution remain planned or explicitly rejected by
existing policy coverage.

## Directory And Lifecycle Impact

No directory layout changes. Native InnoDB performs the in-place index
operations inside the MyLite database directory, and the existing final reopen
checks prove that durable table/index metadata survives ownerless/native reopen
before and after forced shared-memory rebuild.

## Native Storage Impact

The change relies on MariaDB/InnoDB native in-place alter behavior. MyLite's
tested responsibility is dictionary-generation publication and peer refresh
across completed add/drop boundaries.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `embedded-dev`.
- Run the focused selector:
  `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test online-ddl-options`.
- Run the same selector in the hook build.
- Run the relevant embedded CTest shard for ownerless SQL coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The already-open ownerless peer observes the `INPLACE, LOCK=SHARED` dropped
  secondary index and cannot use it through `FORCE INDEX`.
- The peer observes the `INPLACE, LOCK=SHARED` re-added secondary index and can
  use it through `FORCE INDEX`.
- Later final-state checks still prove the index can be removed by the existing
  `NOCOPY, LOCK=NONE` path and does not reappear after ownerless/native reopen
  or forced `.shm` rebuild.
- Compatibility docs and the ownerless concurrency spec name the new coverage
  while keeping broader randomized DDL oracles and SQL table-wait fault
  injection planned.

## Risks And Unresolved Questions

- This remains deterministic option-matrix coverage, not exhaustive MariaDB DDL
  exploration.
- SQL-level table-lock wait fault injection remains planned because existing
  SQL shapes have not reached the ownerless table-wait callback.
- Full external MariaDB/RQG long-running DDL oracle execution remains
  environment-owned follow-up work.
