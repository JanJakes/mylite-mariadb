# Direct Update Inner Row Write

## Goal

Reduce the accepted exact-key prepared `UPDATE` direct path by avoiding the
redundant nested `handler::ha_update_row()` wrapper inside
`ha_mylite::direct_update_rows()`.

The direct-update hook should still use the existing MyLite row update
implementation and fall back for table shapes that require private MariaDB
in-server update checks outside the storage-engine API.

## Non-Goals

- New direct-update statement shapes.
- Direct `DELETE`.
- Bypassing MariaDB assignment, condition, no-op, CHECK, hidden-index, or
  supported unique constraint semantics.
- Supporting row-binlogged direct updates, `UPDATE IGNORE`, triggers, views, or
  unsupported key-changing shapes.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/handler.cc::handler::ha_direct_update_rows()` already wraps the
  storage-engine direct hook with `MYSQL_UPDATE_ROW_START/DONE` and
  `mark_trx_read_write()`.
- `mariadb/sql/handler.cc::handler::ha_update_row()` adds the per-row wrapper
  normally used by the generic update loop: in-server unique/overlap checks,
  `MYSQL_UPDATE_ROW_START/DONE`, `mark_trx_read_write()`, handler update
  statistics, `TABLE_IO_WAIT`, the virtual `update_row()` call, hidden-index
  maintenance, row-binlog publication, and WSREP hooks.
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` only uses
  direct update when row binlogging is not active, matching MyLite's embedded
  no-binlog profile.
- `handler::ha_check_inserver_constraints()` is private to `handler`, so MyLite
  must not call it directly from the storage engine.
- MyLite direct update already rejects `UPDATE IGNORE`, triggers, views, BLOB
  rows, volatile rows, unique-key-changing updates, and FK-sensitive key
  changes. This slice also rejects table shapes that require private in-server
  update checks, such as long unique hash and `WITHOUT OVERLAPS` constraints.

## Design

Add a MyLite-owned inline row-write sequence for the already accepted
direct-update row. It will:

1. Assert that direct admission rejected table shapes that need private
   in-server update constraints.
2. Call `ha_mylite::update_row()` directly with `record[1]` and `record[0]`.
3. Run `table->hlindexes_on_update()`.
4. Increment `rows_stats.updated`.

The row-write sequence will not duplicate `MYSQL_UPDATE_ROW_START/DONE` or
`mark_trx_read_write()` because `ha_direct_update_rows()` already provides the
direct-operation wrapper. It will not publish row-binlog events because the SQL
layer does not enter the direct branch for row-binlogged statements and MyLite's
embedded profile disables binlog runtime.

## Compatibility Impact

No new SQL surface is supported. Accepted direct updates should keep the same
values, diagnostics, affected-row counts, rollback behavior, and supported
index maintenance as the current direct-update implementation.

Unsupported shapes continue to fall back through MariaDB's ordinary update
loop.

## File Lifecycle

No file-format or sidecar changes. The direct sequence still calls MyLite's
existing row update implementation, so durable row, row-state, index-entry,
rollback journal, and cache behavior remain unchanged.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive with static MyLite
  storage.
- Build the storage-smoke embedded storage-engine test and performance tool.
- Run focused storage-smoke tests covering prepared primary-key update rebinds,
  no-op affected rows, secondary-index maintenance, duplicate fallback, CHECK,
  generated, and FK update regressions.
- Run the prepared update performance baseline and sample the accepted direct
  path to confirm it still enters `ha_mylite::direct_update_rows()`.
- Run `git diff --check` and `git clang-format --diff` on touched C/C++ files.

## Acceptance Criteria

- Focused storage-smoke tests pass.
- The accepted prepared point-update profile keeps the direct-update hook and
  no longer samples the nested `handler::ha_update_row()` wrapper.
- Prepared update performance is measured on the local storage-smoke benchmark
  and does not show a material regression.
- The diff remains scoped to MyLite handler code, tests, docs, and roadmap
  notes.

## Risks And Open Questions

- `ha_update_row()` performs more than a virtual dispatch; the direct sequence
  must keep hidden-index maintenance and must not admit table shapes that require
  private in-server constraint checks.
- Row-binlog and WSREP hooks are intentionally not reproduced because the
  direct branch is not used for row-binlogged statements and WSREP is outside
  MyLite's embedded profile.
- Wider use of this sequence should wait until direct update supports broader
  statement shapes.
