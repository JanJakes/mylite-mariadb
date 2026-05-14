# Copy ALTER Rebuilds

## Problem

MyLite can create, discover, drop, rename, insert, scan, update, and delete
keyless routed tables, but `ALTER TABLE` is still documented as planned. Real
schemas need at least the copy-rebuild path for column additions, column
renames, type-compatible column changes, and explicit table rebuilds before
index metadata and transaction work expand the state space.

This slice should not invent a MyLite-specific ALTER executor. MariaDB already
owns SQL parsing, column mapping, defaults, virtual column evaluation, warnings,
and copy-row semantics. MyLite should participate through the handler boundary
and keep durable state in the primary `.mylite` file.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:create_table_impl()` builds the canonical table
  definition and calls `ha_create_table()` for non-`frm_only` creates.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` creates an intermediate
  table, opens it, copies rows, then renames the old table to a backup name and
  the new table to the final name.
- `mariadb/sql/sql_table.cc:copy_data_between_tables()` maps old fields to new
  fields with `Copy_field`, applies defaults and virtual fields, and writes
  rebuilt rows through `to->file->ha_write_row()`.
- `mariadb/sql/sql_table.cc:online_alter_check_supported()` allows engine-wide
  opt-out from online copy ALTER through `HA_NO_ONLINE_ALTER`.
- `mariadb/sql/handler.cc:ha_create_table()` initializes a `TABLE_SHARE` from
  the generated `.frm` image before calling the engine's `create()` method.
- `mariadb/storage/mylite/ha_mylite.cc` already stores generated definition
  images in the MyLite catalog and implements handler `rename_table()` and
  `delete_table()`, which match MariaDB's copy ALTER rename/cleanup phases.
- MariaDB documentation describes `ALGORITHM=COPY` as the original ALTER TABLE
  copy algorithm and notes that recent MariaDB versions can run many copy
  alters online unless the engine or operation prevents it:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/alter/alter-table>.

## Design

Support copy ALTER for MyLite-routed base tables whose rebuilt table shape is
still supported by the current keyless row lifecycle:

- keyless tables without `AUTO_INCREMENT`,
- omitted/default engine and explicit `ENGINE=MYLITE`, `ENGINE=InnoDB`,
  `ENGINE=MyISAM`, and `ENGINE=Aria` requests that route to MyLite.

The implementation should rely on MariaDB's copy path:

1. MariaDB creates a temporary MyLite table definition.
2. MyLite stores that temporary table definition in the catalog with a fresh
   table id.
3. MariaDB copies rows through `ha_write_row()`, which serializes the rebuilt
   MariaDB record image through the existing MyLite row-payload path.
4. MariaDB renames the old table to a backup name and the temporary table to
   the final name; MyLite catalog `rename_table()` preserves each table id.
5. MariaDB drops the backup; MyLite removes its live catalog record while old
   row pages remain orphaned until free-space management exists.

The handler must advertise `HA_NO_ONLINE_ALTER`. MyLite does not yet have
transaction, online-change-log, locking, or recovery support, so `LOCK=NONE`
must fail explicitly instead of appearing to support online DDL.

`ha_mylite::create()` should reject unsupported target table shapes before
catalog publication. This prevents `ALTER ... ADD PRIMARY KEY` or other general
index rebuilds from creating a catalog entry that later cannot be maintained.

For `ALTER TABLE` without an explicit `ENGINE=...`, the rebuilt catalog record
must preserve the original requested engine name. For explicit engine changes,
the requested engine should be the explicit SQL engine name while the effective
engine remains `MYLITE`.

## Scope

- `ALTER TABLE ... ALGORITHM=COPY` over supported MyLite-routed table shapes.
- Column add, drop, rename, and type-compatible modify operations handled by
  MariaDB's row-copy machinery.
- Explicit rebuilds such as `ALTER TABLE ... ENGINE=InnoDB, ALGORITHM=COPY`.
- BLOB/TEXT and NULL values surviving the copy rebuild.
- Catalog discovery, metadata, and row visibility after close/reopen.
- Failed unsupported rebuilds must leave no live temporary catalog table.

## Non-Goals

- `LOCK=NONE` or online copy ALTER.
- In-place, instant, or no-copy ALTER algorithms.
- Adding primary, unique, secondary, FULLTEXT, SPATIAL, or generated-column
  indexes.
- Foreign keys, triggers, check-constraint expansion, and general generated
  column edge cases beyond what MariaDB evaluates during the copy.
- Transaction rollback or crash recovery for failed or interrupted ALTER.
- Free-space reclamation of old table-definition blobs or old row pages.

## Compatibility Impact

`ALTER TABLE` moves from planned to partial for supported copy rebuilds. MyLite
still cannot claim general ALTER compatibility because index maintenance,
online DDL, rollback, crash recovery, and foreign-key semantics remain planned.

## Single-File Impact

The rebuilt table definition and rebuilt rows are appended to the primary
`.mylite` file. The old table's definition blob and row pages become orphaned
after the backup catalog record is dropped. No durable MariaDB `.frm`, `.ibd`,
`.MYD`, `.MYI`, `.MAI`, `.MAD`, or Aria log sidecars are introduced.

## Embedded Lifecycle And API

No public `libmylite` API is added. Successful ALTER statements should surface
through existing direct execution diagnostics. Failed unsupported ALTER
statements should return MariaDB errors and preserve the previous catalog
state.

## File Format Impact

No new page type is required. Copy ALTER uses existing table-definition blob
pages, row pages, row-payload blob pages, autoincrement state pages, and catalog
rename/drop records.

## Storage-Engine Routing Impact

Current routing continues to map omitted/default engine, `MYLITE`, `InnoDB`,
`MyISAM`, and `Aria` to MyLite. The catalog must preserve requested engine
metadata across rebuilds so `ENGINE=InnoDB` remains recorded as requested
`InnoDB` even though the effective engine is `MYLITE`.

## Test Plan

- Extend storage-engine smoke coverage for:
  - keyless `ALTER TABLE ... ADD COLUMN ... ALGORITHM=COPY`,
  - keyless column rename/type-compatible modification,
  - keyless column drop,
  - explicit same-engine rebuild with `ENGINE=InnoDB`,
  - BLOB/TEXT and NULL row values before and after close/reopen,
  - requested/effective engine metadata after ALTER,
  - `LOCK=NONE` failure through the handler's online ALTER gates,
  - unsupported index rebuild failure with no temporary catalog leak,
  - no forbidden durable sidecars.
- Run normal dev, embedded, storage-smoke, format, diff, tidy, and MariaDB
  archive checks.

## Acceptance Criteria

- Supported keyless copy ALTER statements complete through MariaDB SQL
  execution.
- Rebuilt rows are visible before and after close/reopen.
- Requested engine metadata survives implicit rebuilds and updates on explicit
  engine requests.
- Unsupported indexed rebuilds fail before publishing a live MyLite catalog
  table.
- `LOCK=NONE` copy ALTER fails explicitly.
- Compatibility, roadmap, and storage docs describe partial ALTER support and
  remaining limits.

## Risks

- The current append-only storage layer cannot roll back a failed multi-step
  ALTER after header publication points; transaction and recovery work must
  close that gap before durability claims.
- MariaDB can evaluate many column transformations during copy, but MyLite
  should not claim broad generated-column, constraint, or index behavior until
  those surfaces have targeted tests.
- Old pages are orphaned after rebuilds. Free-space management and compaction
  are separate storage slices.
