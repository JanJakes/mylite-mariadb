# Ownerless Schema Drop Loop Live Recovery

## Problem Statement

`DROP DATABASE` removes base tables by walking the schema's table list before it
logs and removes the schema directory itself. Existing ownerless recovery
covered a completed table-bearing schema drop killed at the dictionary-finish
boundary, but not a crash inside MariaDB's internal table-drop loop. MyLite must
prove a live ownerless peer can recover the durable partial state without
inventing either full schema removal or full rollback.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_db.cc` `mysql_rm_db_internal()` discovers schema tables with
  `find_db_tables_and_rm_known_files()`, locks them, then calls
  `mysql_rm_table_no_locks()` with the shared `DDL_LOG_STATE` before calling
  `ddl_log_drop_db()`.
- `find_db_tables_and_rm_known_files()` removes deletable non-table files such
  as `db.opt` before the table-drop loop, so the crash boundary leaves the
  schema directory present but its option file already removed.
- `mariadb/sql/sql_table.cc` `mysql_rm_table_no_locks()` logs each
  `DDL_LOG_DROP_TABLE_ACTION`, deletes native engine files and `.frm`, and only
  then advances to the next `TABLE_LIST` entry. The MyLite
  `mylite_ownerless_dictionary_native_file_op()` hook runs after each
  successful non-temporary base-table drop.
- `mariadb/sql/ddl_log.cc` `ddl_log_drop_db()` is written only after the table
  loop completes. A crash after one table's native file operation and before
  the loop completes therefore has durable table-drop progress but no durable
  schema-drop entry.

## Design

Reuse the existing SQL-layer ownerless dictionary native-file hook. Add a
focused hook-build selector that creates a schema with two InnoDB tables and
kills:

```sql
DROP DATABASE ownerless_schema_drop_loop_crash
```

at `drop-table-after-native-file-op`. Because MariaDB's table discovery is
directory-derived, the test accepts either table as the first dropped entry and
proves exactly one table remains. The remaining table must be readable and
writable while another ownerless peer is still live, the schema directory must
remain present, `db.opt` must already be absent, and final no-live recovery
must drain the native file-operation marker.

## Scope And Non-Goals

In scope:

- Two-table InnoDB schema drop killed after the first table's native file
  operation.
- Live-peer recovery of the durable partial schema state.
- Native file-operation marker retention while a peer remains live and marker
  drain after final no-live recovery.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild.

Out of scope:

- Complete `DROP DATABASE` recovery after `ddl_log_drop_db()`.
- Routines, events, views, triggers, partitions, and mixed engine schema
  contents.
- Reordering or completing later unlogged schema-drop work in an already-live
  peer.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL syntax or public API behavior changes. The covered behavior follows
MariaDB's DDL-log ordering: a crash before `ddl_log_drop_db()` preserves the
schema and later unlogged tables while keeping already logged native table-drop
progress.

## Directory, Lifecycle, And Native Storage Impact

No durable layout change. The test proves ownerless live recovery preserves the
schema directory, observes MariaDB's early `db.opt` removal, leaves exactly one
native table absent, keeps the remaining table's `.frm`/`.ibd` files usable,
and drains the
native-file-operation marker only after no live ownerless peer remains.

## Public API, Build, Size, License

No public API, dependency, binary-size, or license changes. The slice adds
hook-build test coverage and documentation only.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector `dictionary-schema-drop-loop-crash`.
- Run the adjacent dictionary drop hook CTest group.
- Run production formatting and CI-production-build guards.
- Run `git diff --check`.

## Acceptance Criteria

- The killed writer stops inside `mysql_rm_db_internal()`'s table drop loop
  after one native table drop and before `ddl_log_drop_db()`.
- A live ownerless opener recovers the partial state while the native
  file-operation marker remains set.
- The schema directory remains present and `db.opt` is absent.
- Exactly one table is absent and exactly one table remains readable and
  writable.
- Final no-live recovery clears the native file-operation marker.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  preserve the same partial-progress state.

## Risks And Follow-Up

- The selector covers a two-table InnoDB schema. Broader schema contents such as
  routines, events, views, triggers, partitions, and mixed engines still need
  broader DDL/file-lifecycle evidence.
- The test intentionally follows MariaDB's durable progress. It does not make
  `DROP DATABASE` atomic across a crash.
