# Truncate Table Lifecycle

## Problem

MyLite supports routed table creation, row insert, scans, update/delete,
copy rebuilds, autoincrement state, and supported indexes, but
`TRUNCATE TABLE` still falls through the base handler unsupported path.
That leaves a common MySQL/MariaDB DDL operation unusable for MyLite-routed
tables, including tables declared as `ENGINE=InnoDB` that MyLite resolves to
the `MYLITE` storage engine.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_truncate.h` defines `Sql_cmd_truncate_table` for
  `TRUNCATE TABLE` execution.
- `mariadb/sql/sql_truncate.cc:handler_truncate()` opens and locks the target
  table, checks foreign-key parent restrictions, and calls
  `table->file->ha_truncate()`.
- `mariadb/sql/sql_truncate.cc:truncate_table()` uses the handler truncate
  method when the storage engine does not advertise `HTON_CAN_RECREATE`.
  MyLite does not advertise `HTON_CAN_RECREATE`, so its correct integration
  point is the handler method rather than drop/recreate.
- `mariadb/sql/handler.cc:handler::ha_truncate()` marks the operation as a
  write and calls the engine's `truncate()` override.
- `mariadb/sql/handler.h:handler::truncate()` documents that the engine is
  responsible for implementing MySQL's `TRUNCATE TABLE` DDL operation and
  resetting the autoincrement counter.
- `mariadb/sql/handler.h:handler::delete_all_rows()` returns
  `HA_ERR_WRONG_COMMAND` by default, which is the current MyLite inherited
  behavior.

## Scope

- Routed base tables using the MyLite handler, including requests recorded as
  `DEFAULT`, `MYLITE`, `InnoDB`, `MyISAM`, and `Aria`.
- Keyless and supported keyed tables, including primary, unique, and secondary
  index entries.
- BLOB/TEXT row payloads.
- Autoincrement reset to the first generated value after truncate.
- Catalog metadata preservation, close/reopen visibility, and sidecar gates.

## Non-Goals

- Foreign-key support beyond MariaDB's current pre-handler truncate checks.
- Transaction rollback, savepoints, statement rollback, or crash recovery for
  SQL-level truncate semantics beyond the existing storage rollback journal.
- Physical page reclamation, B-tree index truncation, or compaction.
- `ALTER TABLE ... AUTO_INCREMENT=N`; this slice resets autoincrement only as
  part of `TRUNCATE TABLE`.
- Temporary table storage redesign.

## Design

Implement truncate at the MyLite handler/storage boundary:

1. MariaDB parses and authorizes `TRUNCATE TABLE`.
2. MariaDB opens the routed MyLite table and calls `ha_mylite::truncate()`.
3. The handler validates the same supported row/index shape used by
   row-lifecycle operations, calls a first-party storage truncate API, clears
   cached cursors, and returns the mapped handler status.
4. The storage layer resolves the live catalog table id, scans currently live
   row ids through the existing row-state map, and appends delete row-state
   pages for those live row ids.
5. The same publication appends an autoincrement state page with next value
   `1`, unless the table is already empty and the current value is already
   `1`.
6. The header page count is advanced once after the delete-state and
   autoincrement pages are written, under the existing rollback-journal
   publication path.

This is a logical truncate. Old row, BLOB overflow, index-entry, and old
autoincrement pages remain in the primary file until compaction exists, but
all scans and index reads filter them through current row-state and table-id
state. The catalog record and table id are preserved, matching truncate's
table-definition-preserving semantics.

## Compatibility Impact

`TRUNCATE TABLE` moves from planned to partial for supported routed table
shapes. It remains partial because MyLite still lacks SQL transaction
rollback, foreign-key enforcement, physical compaction, and broader
transaction-aware index maintenance.

## DDL Metadata Routing Impact

Truncate does not create, drop, rename, or rebuild table metadata. The MyLite
catalog row remains unchanged, including requested engine metadata such as
`InnoDB` resolving to effective `MYLITE`.

## Single-File And Embedded Lifecycle Impact

All durable truncate state is appended to the primary `.mylite` file. The
operation must not create persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`,
`.MAD`, `aria_log.*`, binlog, relay log, or plugin-owned table files.

The existing rollback journal is the only companion touched by the storage
publication path. It must be removed after successful truncate and recovered
through existing open paths after interrupted publication.

## Public API And File Format Impact

Add an internal storage-package API:

```c
mylite_storage_result mylite_storage_truncate_table(
    const char *filename,
    const char *schema_name,
    const char *table_name
);
```

No `libmylite` public API changes. No new page type is needed; truncate reuses
row-state and autoincrement pages. The storage capability mask gains a
truncate capability flag.

## Storage-Engine Routing Impact

`TRUNCATE TABLE` is accepted for supported MyLite-routed tables regardless of
whether the original SQL requested `MYLITE`, omitted/default engine,
`InnoDB`, `MyISAM`, or `Aria`. Unsupported table shapes remain explicit
handler failures before any storage publication.

## Binary-Size And Dependency Impact

No new dependency is introduced. The binary-size impact should be limited to a
small storage API and handler override; update the measured size table after
verification.

## Test Plan

- Add storage unit coverage for:
  - truncating a table with live rows and supported index entries;
  - hiding old row ids and old index entries;
  - resetting autoincrement to `1`;
  - preserving catalog metadata and close/reopen visibility;
  - no-op truncate on an already empty table.
- Add storage-engine smoke coverage for:
  - `TRUNCATE TABLE` over an `ENGINE=InnoDB` routed keyed/autoincrement table;
  - generated autoincrement after truncate returning to `1`;
  - old unique keys becoming reusable;
  - forced index reads after new inserts;
  - close/reopen persistence;
  - catalog metadata and durable-sidecar gates.
- Run the normal dev, embedded-dev, storage-smoke-dev, format, tidy, diff,
  shell, compatibility harness, and size checks.

## Acceptance Criteria

- `TRUNCATE TABLE` succeeds for supported routed MyLite tables.
- Rows and index entries visible before truncate are invisible after truncate.
- New inserts after truncate work through scans and supported indexes.
- Autoincrement generation restarts at `1`.
- Requested/effective engine metadata is preserved.
- Docs, compatibility matrix, and roadmap describe truncate as partial support
  with explicit remaining limits.

## Implementation Status

Implemented in the storage package and MyLite handler:

- `mylite_storage_truncate_table()` appends delete row-state pages for live
  row ids and resets table-local autoincrement state in one rollback-journal
  publication.
- `ha_mylite::truncate()` handles MariaDB's `handler::ha_truncate()` call for
  supported MyLite-routed table shapes.
- Storage unit coverage checks row, index-entry, autoincrement, metadata, and
  no-op-empty-table behavior.
- Storage-engine smoke coverage checks routed `ENGINE=InnoDB` SQL truncate,
  unique-key reuse, forced-index reads, autoincrement reset, close/reopen, and
  sidecar gates.

## Risks And Open Questions

- Logical truncate is O(live rows) and appends one row-state page per live row.
  That is acceptable before compaction and physical index structures exist.
- SQL-level rollback is still not implemented; a successful truncate is
  published as a committed storage mutation.
- Future foreign-key support must revisit truncate enforcement, but current
  MyLite tables do not claim foreign-key compatibility.
