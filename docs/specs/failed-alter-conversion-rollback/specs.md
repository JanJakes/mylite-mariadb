# Failed ALTER Conversion Rollback

## Goal

Cover failed copy `ALTER TABLE ... MODIFY COLUMN` rollback when MariaDB rejects
existing row data during strict type conversion. The failed ALTER must preserve
the old MyLite-routed table definition, rows, indexes, catalog metadata, and
close/reopen visibility.

## Non-Goals

- Do not add general transactional DDL.
- Do not change MariaDB type-conversion or SQL-mode behavior.
- Do not cover every type-family conversion matrix.
- Do not change public `libmylite` APIs, file format, or binary profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:12056-12061` routes copy ALTER rebuilds through
  `copy_data_between_tables()`.
- `mariadb/sql/sql_table.cc:12740-12741` sets `abort_on_warning` for strict
  non-`IGNORE` copy ALTER operations.
- `mariadb/sql/sql_table.cc:12767-12790` builds copy fields from old columns
  to the altered table definition.
- `mariadb/sql/sql_table.cc:12944-12969` copies each old row into the altered
  table, then aborts when the thread enters an error state.
- `mariadb/sql/field.cc:4034-4071` stores `TINYINT` values and raises
  `ER_WARN_DATA_OUT_OF_RANGE` when an assigned value exceeds the target range.
- `packages/libmylite/src/database.cc:1563-1583` wraps file-backed top-level
  storage DDL, including `ALTER`, in a MyLite statement checkpoint.
- `packages/libmylite/src/database.cc:1589-1602` rolls that checkpoint back
  when MariaDB reports execution failure.

## Design

Add storage-smoke coverage for a routed `ENGINE=InnoDB` table with an ordinary
`INT` column, rows that exceed `TINYINT UNSIGNED`, and supported indexes. Under
`STRICT_ALL_TABLES`, attempt:

```sql
ALTER TABLE conversion_posts
MODIFY COLUMN rating TINYINT UNSIGNED NOT NULL,
ALGORITHM=COPY
```

MariaDB should reject the rebuild while copying existing rows. MyLite should
roll back the statement checkpoint so the old `INT` definition and all visible
rows/indexes remain usable in the same runtime and after close/reopen.

## Compatibility Impact

This narrows the roadmap's broader SQL rollback gap for a common copy-ALTER
failure shape. It does not claim full transactional DDL, savepoint-aware DDL,
or exhaustive type-conversion compatibility.

## DDL Metadata Routing Impact

The failed ALTER must not publish the attempted `TINYINT UNSIGNED` table
definition. Requested `InnoDB` metadata and effective `MYLITE` metadata must
remain unchanged.

## Single-File And Embedded Lifecycle

No new durable companion files are introduced. Pages appended during the failed
copy ALTER remain unreachable until compaction exists, while the visible
header/catalog state is restored by the existing checkpoint.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

The smoke test uses explicit `ENGINE=InnoDB`, which routes to MyLite in the
default embedded profile. The same checkpoint path applies to other routed
requested engines, but this slice covers one representative compatibility
target.

## Build, Size, And Dependencies

No dependency or build-profile change. Binary-size impact is limited to test and
documentation text unless the coverage exposes a production bug.

## Test Plan

- Add storage-engine smoke coverage for strict failed copy ALTER conversion
  rollback.
- Verify the old rows, forced secondary-index reads, requested/effective engine
  metadata, and new valid `INT` inserts remain available after the failure.
- Repeat key checks after close/reopen and assert durable sidecar gates.
- Run focused storage-smoke, statement-rollback, routed DDL/DML, shell syntax,
  reject-file, and whitespace checks.

## Acceptance Criteria

- Strict copy ALTER from `INT` to `TINYINT UNSIGNED` fails on out-of-range
  existing rows.
- The failed ALTER leaves the original table definition and rows visible before
  and after close/reopen.
- Existing secondary indexes remain usable after the failed ALTER.
- Compatibility, roadmap, storage architecture, and harness docs identify this
  rollback shape as covered while keeping broader SQL rollback planned.

## Risks And Open Questions

- This proves one strict numeric conversion failure, not every string, temporal,
  collation, generated-column, or multi-column conversion path.
- The coverage proves visible statement rollback, not crash-safe logical undo if
  the process dies in the middle of the failed ALTER.
