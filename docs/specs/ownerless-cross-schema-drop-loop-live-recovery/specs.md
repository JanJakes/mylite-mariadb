# Ownerless Cross-Schema Drop Loop Live Recovery

## Problem Statement

The same-schema drop-loop recovery slice proved a crash after the first
`DROP TABLE` entry's native file operation and before the next table was logged
or dropped. The remaining multi-drop loop gap included cross-schema table lists,
where durable progress crosses schema directories and must not accidentally
complete or roll back later unlogged entries while a live ownerless peer remains
open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc` `mysql_rm_table_no_locks()` iterates the
  `TABLE_LIST` in statement order, logs a `DDL_LOG_DROP_TABLE_ACTION`, removes
  the engine table and `.frm`, then advances to the next table.
- `mariadb/sql/ddl_log.cc` replays durable DROP log entries only for operations
  that were written before the crash. A crash after the first cross-schema table
  drop but before the second table is logged is therefore a partial-progress
  state: the first schema/table pair is absent and the later schema/table pair
  remains present.
- The MyLite SQL-layer dictionary hook now marks native file-operation DDL
  recoverable after each completed native base-table drop. This slice verifies
  that hook across schema directories.

## Design

Reuse the existing `mylite_ownerless_dictionary_native_file_op()` hook in
`mysql_rm_table_no_locks()`. Add a focused hook-build test that kills:

```sql
DROP TABLE app.ownerless_cross_multi_drop_loop_crash_a,
  ownerless_cross_multi_drop_loop_schema.ownerless_cross_multi_drop_loop_crash_b
```

after the first native file operation. Live-peer recovery should expose the
durable partial progress: the `app` table is absent, the other schema still
exists, and the other schema's table remains readable and writable.

## Scope And Non-Goals

In scope:

- Cross-schema two-table `DROP TABLE` killed after the first table's native
  file-operation boundary.
- Live-peer recovery of the partial native state.
- Native file-operation marker retention while a peer remains live and no-live
  drain after release.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `DROP DATABASE` internal table-list loops.
- Mixed views, triggers, temporary tables, and `IF EXISTS` variants.
- Completing unlogged future DDL entries in an already-live peer.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL syntax or public API behavior changes. The covered crash boundary follows
MariaDB's durable DDL-log progress semantics for a cross-schema table-list drop:
completed entries remain applied and later unlogged entries remain present.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or durable format changes. The test proves live ownerless
recovery preserves the absent first table's `.frm`/`.ibd` files and the present
second table's schema directory and native files until final no-live recovery
drains the marker.

## Public API, Build, Size, License

No public API, dependency, binary-size, or license changes. The slice adds only
hook-build test coverage and docs.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector
  `dictionary-cross-schema-multi-drop-loop-crash`.
- Run the adjacent multi-drop hook CTest group.
- Run production formatting and CI-production-build guards.
- Run `git diff --check`.

## Acceptance Criteria

- The killed writer stops after dropping the first table in `app` and before
  the second schema/table pair is logged or dropped.
- A live peer recovers dictionary state while the native file-operation marker
  remains set.
- The first table is absent; the second schema and table remain present,
  readable, and writable.
- Final no-live recovery clears the native file-operation marker.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  preserve the same partial-progress state.

## Risks And Follow-Up

- This proves cross-schema `DROP TABLE` loop progress only. `DROP DATABASE`
  internal loops remain planned.
- The hook still exposes only MariaDB's durable progress; it does not replay
  later unlogged table-list entries in an already-live peer.
