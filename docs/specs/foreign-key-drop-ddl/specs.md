# Foreign-Key Drop DDL

## Goal

Support `ALTER TABLE ... DROP FOREIGN KEY` for the public MyLite
`RESTRICT` / `NO ACTION` foreign-key subset.

The slice completes the first FK metadata lifecycle step: a constraint created
through supported `CREATE TABLE` or copy `ALTER TABLE ... ADD FOREIGN KEY` can
be removed by name, disappears from MyLite FK metadata and `SHOW CREATE TABLE`,
and no longer participates in child or parent row checks after close/reopen.

## Non-Goals

- Cascades, `SET NULL`, `SET DEFAULT`, and full InnoDB FK behavior.
- Treating `foreign_key_checks=0` as a dump-import bypass.
- Advertising `HTON_SUPPORTS_FOREIGN_KEYS`.
- Automatically dropping child supporting indexes when a FK is dropped.
  MariaDB's rebuilt table definition remains the authority for retained keys;
  generated-key cleanup can be a later compatibility slice.
- Durable DDL inside active MyLite transactions.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... DROP FOREIGN KEY` into an
  `Alter_drop(Alter_drop::FOREIGN_KEY, ...)` and sets
  `ALTER_DROP_FOREIGN_KEY`.
- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` keeps FK drops in
  `alter_info->drop_list`, validates missing non-`IF EXISTS` FK names through
  `handler::get_foreign_key_list()`, and excludes dropped FK metadata when it
  rebuilds old FK definitions into `new_key_list`.
- `mariadb/sql/sql_table.cc:fk_check_column_changes()` removes dropped FKs from
  the compatibility checks for altered child columns.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::rename_table()` detects
  MariaDB's copy-ALTER old-table backup rename and calls
  `mylite_storage_rename_table_for_rebuild_backup()` so FK records attached to
  the logical final table survive ordinary copy rebuilds.
- `packages/mylite-storage/src/storage.c:mylite_storage_drop_foreign_key_definition()`
  already removes a named FK catalog record under the rollback-journal
  publication path.
- Before this slice,
  `packages/libmylite/src/database.cc:unsupported_foreign_key_sql_message()`
  rejected `ALTER TABLE ... DROP FOREIGN KEY` before MariaDB execution.

## Compatibility Impact

`DROP FOREIGN KEY` moves from explicitly unsupported to partial support for
validated MyLite-routed durable base tables. MariaDB continues to validate
missing names and `IF EXISTS` warning behavior through the handler FK-list
hook. Dropping a FK removes referential checks but does not claim InnoDB's full
index cleanup or online/in-place ALTER semantics.

## Design

1. Remove the `libmylite` SQL policy rejection for
   `ALTER TABLE ... DROP FOREIGN KEY` while keeping temporary-table FK create
   rejection intact.
2. During `ha_mylite::rename_table()` for MariaDB's copy-ALTER old-table backup
   rename, inspect `current_thd->lex->alter_info.drop_list`.
3. For each `Alter_drop::FOREIGN_KEY`, resolve the stored FK constraint name
   with `mylite_storage_list_foreign_keys()` using MariaDB's
   case-insensitive column-identifier comparison.
4. Drop the matching catalog FK record with
   `mylite_storage_drop_foreign_key_definition()` before preserving remaining
   FK records across the backup rename.
5. Let MariaDB's rebuilt table definition decide which ordinary or
   generated child indexes remain. MyLite will keep enforcing supporting-key
   protection for any retained FK metadata.

## File Lifecycle

No new companion files are introduced. FK drops mutate catalog metadata inside
the primary `.mylite` file and use the existing rollback-journal and statement
checkpoint paths. Dropped FK blob pages remain orphaned until free-space
management exists, matching current dropped table-definition and row-page
behavior.

## Embedded Lifecycle And API

`mylite_exec()` and prepared execution should both reach MariaDB for direct
`ALTER TABLE ... DROP FOREIGN KEY`. After close/reopen, dropped FK metadata
must remain absent and row checks must stay disabled for the dropped
constraint.

## Build, Size, And Dependencies

No dependency or profile-size change is expected. The slice adds a narrow
handler bridge and tests.

## Test Plan

- Storage-smoke coverage for dropping a FK created in initial DDL.
- Storage-smoke coverage for dropping a FK created by copy `ALTER`.
- Verify `SHOW CREATE TABLE` and information-schema no longer list the dropped
  constraint.
- Verify child inserts with missing parents and parent update/delete operations
  succeed after the relevant FK is dropped.
- Verify close/reopen preserves the removed metadata.
- Keep missing-name and `IF EXISTS` behavior covered by MariaDB diagnostics and
  warnings.
- Verification: storage-smoke embedded tests, default storage tests,
  format-check, and `git diff --check`.

## Acceptance Criteria

- `ALTER TABLE ... DROP FOREIGN KEY` succeeds for constraints in the supported
  public FK subset.
- Dropped FK metadata is absent from MyLite child and parent FK listings,
  `SHOW CREATE TABLE`, and information-schema rows before and after close/reopen.
- Row checks no longer enforce the dropped constraint.
- Retained FK constraints and their supporting-key protections continue to work.
- Unsupported broader FK actions and import semantics remain documented.

## Risks And Open Questions

- Automatically dropping MariaDB-generated child supporting keys is left for a
  later slice because compatibility depends on MariaDB's retained-key behavior
  and application expectations.
- Multiple FK drops in one ALTER should work through repeated catalog removals,
  but exhaustive multi-drop and mixed ADD/DROP FK matrices can follow the first
  public lifecycle support.
