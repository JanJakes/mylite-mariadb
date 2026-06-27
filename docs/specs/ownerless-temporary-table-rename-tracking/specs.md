# Ownerless Temporary Table Rename Tracking

## Problem Statement

Ownerless SQL tracks connection-local temporary table names so statements that
touch those tables use conservative native reads instead of ownerless
page-version refresh/read shortcuts. Tracking is updated for
`CREATE TEMPORARY TABLE` and `DROP TEMPORARY TABLE`, but not for temporary
table rename forms.

That leaves a correctness and performance gap. If a temporary table shadows a
permanent table and is renamed away, the old name becomes the permanent table
again, but MyLite can keep treating that old name as temporary-table access on
the same handle. That suppresses normal ownerless external refresh for the
permanent table and can also miss conservative handling for the renamed
temporary table's new name.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/temporary_tables.cc:115-138` finds temporary tables in the
  thread's temporary table list; temporary table identity is connection-local.
- `mariadb/sql/temporary_tables.cc:572-606` implements
  `THD::rename_temporary_table()`, renaming a temporary table inside the
  connection-local temporary table list.
- `mariadb/sql/sql_parse.cc:6346-6418` detects whether a `RENAME TABLE` pair
  refers to a temporary table and requires every table in the rename chain to
  have the same temporary/base type.
- `mariadb/sql/sql_table.cc:10085-10133` handles temporary-table `ALTER TABLE`
  rename without the regular non-temporary table rename rollback path.
- `packages/libmylite/src/database.cc:15520` classifies only
  `CREATE TEMPORARY TABLE` and `DROP TEMPORARY TABLE` as temporary-table DDL.
- `packages/libmylite/src/database.cc:17959` updates the ownerless temporary
  name list after successful temporary create/drop, but leaves rename forms
  unchanged.
- `packages/libmylite/src/database.cc:17907` checks later statement identifier
  tokens against that tracked list, so stale old temporary names can suppress
  normal permanent-table refresh after a temp table is renamed away.

## Scope And Non-Goals

In scope:

- Update temporary-table tracking after successful simple
  `RENAME TABLE old TO new` when `old` is a tracked temporary table.
- Update temporary-table tracking after successful simple
  `ALTER TABLE old RENAME TO new` when `old` is tracked.
- Preserve existing `CREATE TEMPORARY TABLE` and `DROP TEMPORARY TABLE`
  behavior, including qualified names.
- Add focused ownerless SQL coverage where a temporary table shadows a
  permanent table, is renamed away, and the same handle must observe peer
  updates to the now-visible permanent table.
- Prove a renamed temporary table still shadows a peer-created permanent table
  with the same new name, and that unrelated ALTER clauses containing a
  `rename` identifier do not mutate temporary-table tracking.

Out of scope:

- Multi-pair temporary `RENAME TABLE` chains.
- Mixed temporary/base rename chains; MariaDB rejects mixed temporary type.
- Hook crash recovery for temporary DDL.
- Partitioned temporary tables and non-InnoDB temporary engines.
- Broader SQL parser replacement; this remains a bounded policy-token fix.

## Design

Add a small token helper that extracts an unqualified table identifier from a
possibly qualified `schema.table` token sequence. Reuse it for temporary
create/drop tracking, then add rename tracking:

- `RENAME TABLE old TO new`: if `old` is a tracked temporary table, remove
  `old` and add `new`.
- `ALTER TABLE old RENAME [TO|AS] new`: if `old` is tracked, remove `old` and
  add `new`.

The focused test starts with a permanent table
`app.ownerless_temp_rename_shadow` and reads it on handle A. Handle A then
creates a same-named temporary table, which shadows the permanent table, and
renames the temporary table to
`app.ownerless_temp_rename_shadow_renamed`. A peer handle updates the permanent
table. Handle A must now read the updated permanent value through the old name,
proving stale temporary tracking was cleared. Handle A also reads the renamed
temporary table through the new name, then a peer creates and updates a
permanent table with the same new name, proving the new temporary name is
tracked for conservative execution. The test also adds a column named
`rename` on the temporary table to ensure the
ALTER rename tracking only handles table-level rename syntax. Finally, it
renames the temporary table back with `ALTER TABLE ... RENAME TO`, drops it,
and verifies the permanent table remains refreshable.

## Compatibility Impact

This aligns MyLite's ownerless temporary-table policy with MariaDB's
connection-local temporary rename behavior. It does not broaden SQL grammar.
It avoids unnecessary conservative ownerless execution for unrelated permanent
tables after a temp table is renamed away, while preserving conservative
handling for the renamed temp table.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. Temporary InnoDB files
remain process/runtime-local. The slice affects only MyLite's in-memory
connection-local temporary table name tracking and the refresh/read policy that
uses it.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The slice
adds one focused ownerless SQL selector and documentation.

## Test And Verification Plan

- Add selector `temporary-table-rename-tracking`.
- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the new selector directly.
- Run adjacent temporary-table selectors:
  `temporary-tablespace` and `temporary-table-crash-recovery`.
- Run ownerless temporary-table stress smoke, production-build guard,
  format-check, and `git diff --check`.

## Acceptance Criteria

- Temporary `RENAME TABLE old TO new` moves the tracked temp name from `old`
  to `new`.
- Temporary `ALTER TABLE old RENAME TO new` moves the tracked temp name from
  `old` to `new`.
- A handle that renames a temp table away from a permanent-table shadow name
  observes peer updates to the now-visible permanent table.
- A renamed temp table continues to shadow a peer-created permanent table with
  the same new name.
- Non-rename ALTER clauses with a `rename` identifier do not move the tracked
  temporary table name.
- Dropping the renamed temporary table clears the tracked table name.
- Existing temporary-table peer and crash-recovery selectors continue to pass.

## Verification Results

Completed on 2026-06-27 with `php-embedded-prod` and `ownerless-stress`
production builds:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test temporary-table-rename-tracking`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-temporary-table-rename-tracking$' --output-on-failure`
- Adjacent production cases:
  `sql-case test_ownerless_temporary_tablespace_allows_peer_temp_tables`,
  `sql-case test_crashed_ownerless_temporary_table_peer_is_recovered`, and
  `temp-stress`.
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-temporary-stress$' --output-on-failure`

## Risks And Follow-Up

- Multi-pair temporary rename chains remain planned.
- Hook crash recovery for temporary DDL remains planned.
- This does not replace the general SQL policy tokenizer; it fixes the bounded
  rename forms used by temporary-table tracking.
