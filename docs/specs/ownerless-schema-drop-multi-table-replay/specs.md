# Ownerless Schema Drop Multi-Table Replay

## Problem

Ownerless stale-reader schema-drop replay already proves retained
reader-boundary WAL does not recreate a dropped schema containing one InnoDB
file-per-table table. `DROP DATABASE` is a schema-wide lifecycle operation, so
the same evidence should cover more than one file-backed table in the removed
schema.

The bounded gap is not a new recovery protocol. It is proving the existing
no-live stale-reader replay policy skips retained page-version records for all
tables removed by the same schema drop, not only the first table shape covered
by the original selector.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_db.cc:mysql_rm_db_internal()` finds all table names in the
  schema directory, locks them, calls `mysql_rm_table_no_locks()` for the table
  list, invokes `drop_database_objects()`, deletes `db.opt`, and removes the
  schema directory.
- `mariadb/sql/sql_db.cc:mysql_rm_db_internal()` reports the number of deleted
  tables after iterating the collected table list, which makes multi-table
  schema drop an ordinary MariaDB path rather than a separate SQL form.
- `docs/specs/ownerless-schema-drop-tablespace-replay/specs.md` records the
  original stale-reader replay design for one table in a dropped schema.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already routes
  `schema-drop-tablespace-replay` through ownerless and ordinary native reopen,
  forced `.shm` rebuild, file absence checks, and WAL checkpoint assertions.

## Scope And Non-Goals

In scope:

- Broaden the existing `schema-drop-tablespace-replay` selector to two InnoDB
  file-per-table tables in the dropped schema.
- Generate retained page-version WAL for both schema-owned tables while a
  repeatable-read peer snapshot pin is live.
- Verify both tables' `.frm` and `.ibd` paths are absent after `DROP DATABASE`
  and remain absent through no-live ownerless/native reopen and forced `.shm`
  rebuild.

Out of scope:

- Durable lifecycle metadata for every DDL class.
- Live-peer DDL/file-lifecycle crash recovery.
- Partitioned schemas, tablespace import/discard, non-InnoDB engines, or
  external randomized DDL/RQG stress.
- Production recovery-policy changes.

## Design

Reuse the existing `schema-drop-tablespace-replay` flow:

1. Create `ownerless_schema_drop_replay`.
2. Create two InnoDB file-per-table tables inside that schema.
3. Insert large rows into both tables and checkpoint cleanly.
4. Hold a peer repeatable-read snapshot pin.
5. Update both tables so retained page-version WAL contains records for both
   soon-to-be-dropped tables.
6. Run `DROP DATABASE ownerless_schema_drop_replay`.
7. Assert the schema, both table metadata entries, and both `.frm`/`.ibd` file
   pairs are absent while WAL remains retained by the live reader pin.
8. Kill the reader and verify no-live ownerless/native reopen plus forced
   `.shm` rebuild continue to observe both tables absent and WAL checkpointed.

## Compatibility Impact

SQL behavior is unchanged. The slice expands partial DDL/file-lifecycle
evidence for ownerless stale-reader recovery after `DROP DATABASE`: retained
page images from multiple dropped schema-owned tables are treated as stale
reader-boundary WAL, not as authority to recreate dropped tables.

## Directory And Lifecycle Impact

No directory layout changes. The selector observes the existing
`datadir/<schema>/`, per-table `.frm`/`.ibd`, ownerless WAL, checkpoint, and
shared-memory rebuild lifecycle.

## Native Storage Impact

No native storage format changes. MariaDB's final native state after the schema
drop remains authoritative when no live ownerless writer recovery evidence
exists.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `schema-drop-tablespace-replay` in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the ownerless SQL CTest shard containing the selector in both presets.
- Run adjacent tablespace replay selectors if the broadened assertions expose
  shared replay behavior.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Both schema-owned tables are updated while the reader pin is live.
- Retained page-version WAL exists after `DROP DATABASE`.
- The dropped schema is absent from `information_schema.schemata`.
- `information_schema.tables` has no rows for the dropped schema.
- Queries against both dropped tables fail.
- Both tables' `.frm` and `.ibd` paths remain absent after no-live ownerless
  reopen, ordinary native reopen, forced `.shm` rebuild, and native reopen
  after rebuild.

## Risks And Follow-Up

- This remains bounded SQL evidence for one multi-table schema drop, not a
  complete durable file-lifecycle metadata design.
- Full live-peer DDL/file-lifecycle crash recovery and external randomized
  MariaDB/RQG stress remain planned separately.
