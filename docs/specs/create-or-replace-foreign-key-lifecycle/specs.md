# CREATE OR REPLACE Foreign-Key Lifecycle

## Goal

Cover `CREATE OR REPLACE TABLE` over MyLite-routed tables that participate in
the supported public foreign-key subset. Replacing a referenced parent table
must fail without dropping rows or FK metadata, while replacing a child table
must remove the old child FK metadata and publish the replacement definition.

## Non-Goals

- Do not add new foreign-key action support.
- Do not cover views, triggers, routines, partitions, temporary tables, or
  multi-table DDL.
- Do not add transactional DDL semantics beyond the existing statement
  checkpoint used by direct file-backed SQL.
- Do not physically reclaim pages made unreachable by the replacement.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:4772-4822` handles base-table
  `CREATE OR REPLACE TABLE` by dropping the existing table through
  `mysql_rm_table_no_locks()` before creating the replacement.
- `mariadb/sql/sql_table.cc:mysql_create_like_table()` preserves
  `OPT_OR_REPLACE` for `CREATE OR REPLACE TABLE ... LIKE`, so LIKE targets
  reuse the same drop-then-create lifecycle.
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` routes
  OR REPLACE CTAS targets through the normal no-lock create path after the old
  target is dropped.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_table()` maps the
  MariaDB drop step to `mylite_storage_drop_table()`.
- `packages/mylite-storage/src/storage.c:mylite_storage_drop_table()` removes
  child FK records when dropping a child table and rejects dropping a referenced
  parent while parent-listable FK records remain.
- `packages/libmylite/src/database.cc:mylite_exec()` wraps direct
  file-backed DDL in a storage statement checkpoint and rolls it back if
  MariaDB returns an error.

## Compatibility Impact

This is a bounded InnoDB-compatible FK lifecycle claim for MyLite's routed
`RESTRICT` / `NO ACTION` subset. A referenced parent table cannot be replaced
by OR REPLACE because replacement would first drop the parent. A child table can
be replaced, and the old child FK metadata disappears with the old child table.

Broader OR REPLACE matrices remain planned, including FK action combinations,
multi-table DDL interactions, temporary table replacement with FKs, unsupported
replacement definitions, and full SQL transaction semantics.

## Design

No production change is expected if the existing paths compose correctly:

1. MariaDB routes OR REPLACE over an existing base table through the same
   drop-then-create path used by ordinary replacement.
2. MyLite's handler drop path rejects a referenced parent before publishing a
   catalog change.
3. The direct SQL statement checkpoint preserves the old parent table if
   MariaDB reports the replacement failure.
4. Replacing a child table uses the child-table drop cleanup path, removing old
   FK records before the replacement definition is stored.

If coverage exposes partial publication, fix the storage or handler path rather
than weakening FK semantics.

## File Lifecycle

The lifecycle stays inside the primary `.mylite` file plus permitted transient
MyLite recovery-journal files. No persistent `.frm`, `.ibd`, `.MYD`, `.MYI`,
`.MAI`, `.MAD`, Aria log, binlog, relay log, or plugin-owned durable table file
is introduced.

## Embedded Lifecycle And API

No public C API change is required. Direct SQL execution exposes the behavior
through existing `mylite_exec()` diagnostics and close/reopen discovery.

## Build, Size, And Dependencies

No dependency or size-profile change is intended. The test uses the existing
storage-smoke embedded build.

## Test Plan

- Create a routed InnoDB parent/child FK pair under `libmylite`.
- Verify `CREATE OR REPLACE TABLE parent (...)` fails while preserving parent
  rows, child rows, catalog metadata, and `INFORMATION_SCHEMA` FK metadata.
- Verify child inserts with missing parents and parent key changes still fail
  after the rejected parent replacement.
- Replace the child table with a MyISAM-routed definition that has no FK.
- Verify the old child rows and FK metadata are gone, orphan child rows can be
  inserted, and parent key changes are no longer blocked.
- Close and reopen the database and verify the replacement child definition,
  parent rows, child rows, and absent FK metadata remain visible without
  durable sidecars.
- Run the focused storage-smoke test, storage-smoke CTest preset, default CTest
  preset, format check, and `git diff --check`.

## Acceptance Criteria

- OR REPLACE over a referenced parent fails.
- Failed parent replacement preserves parent and child table catalog records,
  rows, and FK metadata before and after the failure.
- OR REPLACE over a child table succeeds and removes old child FK metadata.
- The replacement child table is SQL-visible before and after close/reopen.
- No durable MariaDB sidecars appear.

## Implementation Status

Implemented in storage-engine smoke coverage:

- Rejected OR REPLACE over a referenced parent preserves parent/child rows,
  catalog metadata, and FK information-schema metadata.
- Rejected parent replacement leaves child inserts with missing parents and
  parent key changes protected by the original FK.
- OR REPLACE over a child table publishes the replacement child definition,
  removes old child FK metadata, drops old child rows, and allows orphan rows
  plus parent key changes after the FK is gone.
- Close/reopen rediscovers the replacement child table and absent FK metadata
  without durable MariaDB sidecars.
