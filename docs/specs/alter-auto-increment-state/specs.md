# ALTER AUTO_INCREMENT State

## Problem

MyLite persists table-local autoincrement state for inserts, explicit high
values, close/reopen, and truncate reset. `ALTER TABLE ... AUTO_INCREMENT = N`
is still listed as planned, and copy `ALTER TABLE` paths do not have enough
handler support to preserve an already raised counter when the ALTER statement
does not explicitly name a new value.

This slice adds MyLite storage and handler support for the MariaDB
autoincrement ALTER contract while keeping the scope limited to supported
single-column autoincrement table shapes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` copies an existing
  table's autoincrement state into `HA_CREATE_INFO::auto_increment_value` when
  the ALTER did not explicitly set `HA_CREATE_USED_AUTO` and the table has an
  autoincrement field.
- The same preparation path sets `HA_CREATE_USED_AUTO` with value `0` when an
  ALTER drops the autoincrement column.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` calls
  `handler::update_create_info()` before creating the replacement definition,
  so engines can refresh create options from live engine state.
- `mariadb/sql/handler.cc:handler::ha_reset_auto_increment()` calls the engine
  `reset_auto_increment()` hook under a write lock.
- `mariadb/sql/handler.h` documents `reset_auto_increment(value)` as making
  the next inserted row use the given value, and the default `truncate()` path
  resets the counter through `reset_auto_increment(0)`.
- `mariadb/storage/myisam/ha_myisam.cc` reports `HA_STATUS_AUTO`, copies
  `stats.auto_increment_value` in `update_create_info()`, initializes create
  options from `HA_CREATE_INFO::auto_increment_value`, and treats a changed
  explicit `AUTO_INCREMENT` option as incompatible with metadata-only ALTER.
- `mariadb/storage/innobase/handler/ha_innodb.cc` reports
  `HA_STATUS_AUTO`, copies it in `update_create_info()`, initializes newly
  created table state from `info.auto_increment_value` with a default of `1`,
  and forces rebuild for explicit nonzero `AUTO_INCREMENT` ALTER.
- `mariadb/storage/mylite/ha_mylite.cc` currently implements
  `get_auto_increment()` and advances storage state from inserted or updated
  rows, but it does not report `HA_STATUS_AUTO`, override
  `update_create_info()`, initialize create-time autoincrement state, or
  implement `reset_auto_increment()`.

## Design

Add a storage API that publishes an exact next autoincrement value for a table:

- `mylite_storage_set_auto_increment(filename, schema, table, next_value)`
  validates nonzero values, opens the primary file for update, resolves the
  table id, and appends a normal autoincrement page with the requested
  `next_value`.
- Existing `mylite_storage_advance_auto_increment()` keeps its monotonic
  behavior for row writes.
- Existing readers keep using the latest autoincrement page for the table, so
  no file-format change is required.

Wire the MyLite handler to MariaDB's normal autoincrement hooks:

- `ha_mylite::info(HA_STATUS_AUTO)` reads MyLite storage and fills
  `stats.auto_increment_value` for tables with an autoincrement field.
- `ha_mylite::update_create_info()` preserves live state for copy ALTER paths
  that do not explicitly specify `AUTO_INCREMENT`.
- `ha_mylite::create()` initializes the new table's counter from
  `HA_CREATE_INFO::auto_increment_value` after catalog definition publication.
  During copy ALTER, copied rows still go through `write_row()` and can advance
  the counter above the requested value when live row data requires it.
- `ha_mylite::reset_auto_increment()` maps MariaDB's reset value `0` to
  MyLite's first generated value `1` and otherwise publishes the requested next
  value exactly.

Keep MyLite's default `check_if_incompatible_data()` behavior for this slice.
It already forces copy ALTER, which is appropriate while MyLite lacks a
metadata-only ALTER implementation and lets the copied rows preserve row and
index invariants.

## Supported Scope

- `ALTER TABLE ... AUTO_INCREMENT = N` for supported routed MyLite table
  shapes, including after catalog-only close/reopen.
- Preservation of a raised counter across later copy ALTER statements that do
  not specify `AUTO_INCREMENT`.
- Close/reopen persistence of altered autoincrement state.
- Explicit low values are bounded by copied live row values because row copy
  advances the counter from the stored autoincrement field.

## Non-Goals

- Compound autoincrement key edge cases.
- Metadata-only or in-place autoincrement ALTER.
- Transaction-aware rollback of successful explicit autoincrement ALTER.
- Full SQL transaction semantics around autoincrement allocation.
- Changing unsupported ALTER, generated-index, fulltext, spatial, foreign-key,
  or partition behavior.

## Compatibility Impact

MyLite moves `ALTER TABLE ... AUTO_INCREMENT` for supported routed tables from
planned to covered. Behavior follows MariaDB's handler contract: explicit high
values can raise the next generated id, values below existing live row data do
not cause the copy-rebuilt table to reuse ids already present, and omitted
autoincrement options preserve live state.

## DDL Metadata Routing Impact

Copy ALTER continues to publish replacement MariaDB table-definition metadata
through the MyLite catalog. The effective/requested engine routing policy is
unchanged.

## Single-File And Embedded-Lifecycle Impact

No new durable companion files are introduced. Exact autoincrement updates use
the existing autoincrement page type in the primary `.mylite` file and the
existing recovery journal publication path.

## Public API And File-Format Impact

The file format does not change. The first-party storage API gains an exact
autoincrement setter for handler and test use; the public `libmylite` C API is
unchanged.

## Storage-Engine Routing Impact

The behavior applies to all supported routed engine requests because omitted
engine, `ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria`
tables share the MyLite handler and storage layer.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol package changes are included. Future protocol adapters should
inherit the behavior through `libmylite` SQL execution.

## Binary-Size And Dependency Impact

No dependency is added. Size impact is limited to one storage publication API
and small handler hook implementations.

## Test And Verification Plan

- Add storage tests for exact set, lowering, advancing after exact set, and
  reset after drop/recreate.
- Add storage-smoke SQL coverage for explicit high
  `ALTER TABLE ... AUTO_INCREMENT`, preservation across a later copy ALTER,
  close/reopen persistence, reopened explicit changes, and low explicit values
  bounded by copied rows.
- Update compatibility and roadmap docs to remove the planned-only
  `ALTER TABLE ... AUTO_INCREMENT` caveat.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests, and
  relevant compatibility harness groups.

## Acceptance Criteria

- `ALTER TABLE ... AUTO_INCREMENT = N` changes the next generated id for a
  supported routed table.
- Later copy ALTER statements preserve an already raised counter when the SQL
  statement omits `AUTO_INCREMENT`.
- Explicit lower values do not cause copied tables to reuse ids already present
  in live row data.
- The altered state survives `mylite_close()` and reopen.
- The same explicit ALTER path works after catalog-only reopen.
- Docs and compatibility tables match the implemented behavior.

## Risks And Unresolved Questions

- MyLite still lacks transaction-aware autoincrement rollback. Failed covered
  statements are protected by existing statement checkpoints, but full SQL
  transaction semantics remain planned.
- Future metadata-only ALTER support must intentionally call the same exact
  setter or add an equivalent in-place autoincrement update path.
