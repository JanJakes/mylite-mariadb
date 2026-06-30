# Ownerless INPLACE Lock-None Drop DDL

## Problem Statement

Ownerless online DDL option coverage verifies representative `NOCOPY`,
`INPLACE`, `INSTANT`, and `COPY` paths, but the matrix still marks broader
accepted option combinations as planned. The existing `online-ddl-options`
selector adds a secondary index with `ALGORITHM=INPLACE, LOCK=NONE` and keeps
it in the final state. It does not separately prove that an already-open peer
observes a standalone drop of the same option combination and then a re-add
through the same MariaDB path.

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
  `ALGORITHM=INPLACE` and `LOCK=NONE`/`SHARED`/`EXCLUSIVE`, while generated
  column InnoDB tests use `ALGORITHM=INPLACE, LOCK=NONE` for combined
  add/drop-index shapes.
- `packages/libmylite/src/database.cc` wraps ownerless DDL in the dictionary
  begin/finish boundary and refreshes peers when they observe a newer stable
  dictionary generation.

## Design

Extend the existing `online-ddl-options` ownerless SQL selector rather than
adding a second harness. The selector already keeps one ownerless DDL process
and one already-open ownerless peer synchronized at every DDL boundary, then
verifies final state through ownerless reopen, ordinary native exclusive
reopen, forced `.shm` rebuild, and native reopen after rebuild.

After the existing `ALGORITHM=INPLACE, LOCK=NONE` secondary-index add, add two
boundaries:

```sql
ALTER TABLE app.ownerless_ddl_options
  DROP INDEX ownerless_ddl_options_value_cover_idx,
  ALGORITHM=INPLACE, LOCK=NONE;

ALTER TABLE app.ownerless_ddl_options
  ADD INDEX ownerless_ddl_options_value_cover_idx (value, id),
  ALGORITHM=INPLACE, LOCK=NONE;
```

The already-open peer verifies the index metadata is absent after the drop and
usable again after the re-add. Final reopen checks keep proving the re-added
index is durable.

## Compatibility Impact

No SQL semantics change. The slice strengthens evidence that ownerless
read/write peers observe MariaDB-supported in-place/no-lock secondary-index DDL
boundaries for both addition and removal. Unsupported DDL classes and full
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

- The already-open ownerless peer observes the `INPLACE, LOCK=NONE` dropped
  secondary index as absent from metadata.
- The peer observes the `INPLACE, LOCK=NONE` re-added secondary index and can
  use it through `FORCE INDEX`.
- Final ownerless/native reopen checks, including forced `.shm` rebuild, see
  the re-added index and the final row state.
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
