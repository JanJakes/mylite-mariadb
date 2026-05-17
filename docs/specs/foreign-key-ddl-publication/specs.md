# Foreign-Key DDL Publication

## Goal

Enable the first public MyLite foreign-key DDL subset by publishing validated
FK metadata into the primary `.mylite` file and enforcing that metadata through
the existing child/parent row checks. The implemented subset is deliberately
narrow:

- durable MyLite-routed base tables only, including omitted engine and routed
  `ENGINE=InnoDB`;
- `CREATE TABLE` and copy `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY`;
- same-file parent and child tables;
- explicit or MariaDB-generated supported child key prefixes and exact unique
  parent keys;
- immediate `RESTRICT` / `NO ACTION` behavior.

## Non-Goals

- Advertising full InnoDB foreign-key compatibility.
- Cascading actions, `SET NULL`, `SET DEFAULT`, deferrable constraints,
  partitioned tables, foreign keys over volatile zero-file tables, or
  cross-file references.
- Treating `foreign_key_checks=0` as an import bypass.
- Opening arbitrary additional MariaDB `TABLE` objects inside MyLite handler
  row-DML enforcement.
- `ALTER TABLE ... DROP FOREIGN KEY`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses table-level `FOREIGN KEY` and column-level
  `REFERENCES` clauses into `Foreign_key` objects through
  `LEX::add_table_foreign_key()` and `LEX::add_column_foreign_key()`.
- `mariadb/sql/sql_class.h:Foreign_key` stores the parsed constraint name,
  referenced schema/table, child columns, referenced columns, match option, and
  update/delete actions.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table()` validates child
  column existence, fills omitted referenced columns from child columns, and
  leaves FK objects in `Alter_info::key_list`; FK objects are not copied into
  the normal `KEY` array.
- `mariadb/sql/sql_parse.cc:check_fk_parent_table_access()` validates parent
  table privileges and identifier casing for FK DDL. It does not open the
  parent table or validate parent key shape.
- `mariadb/storage/innobase/handler/ha_innodb.cc:
  create_table_info_t::create_foreign_keys()` reads
  `HA_CREATE_INFO::alter_info`, validates child and referenced indexes against
  InnoDB dictionary metadata, stores the referenced key name, and rejects
  unsupported action/key combinations before publishing the constraint.
- `mariadb/sql/handler.h` defines the FK metadata hooks used by SQL-layer DDL,
  information schema, and `SHOW CREATE TABLE`. MyLite already implements those
  hooks over internal catalog metadata without advertising
  `HTON_SUPPORTS_FOREIGN_KEYS`.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::create()` currently stores
  the canonical MariaDB table-definition image before row/autoincrement setup.
  This is the natural point for statement-scoped FK metadata publication, but
  it must not store FKs until validation has succeeded.

Official MariaDB documentation describes foreign keys as storage-engine
referential constraints requiring supporting indexes on the child and parent
tables:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Compatibility Impact

Foreign keys remain partial. Removing the old blanket `libmylite` FK SQL
rejection without this validation would have been a compatibility regression
because MariaDB could accept user-authored FK clauses that MyLite did not
persist or enforce.

The first support claim is that public FK DDL works for validated `RESTRICT` /
`NO ACTION` constraints over explicit or MariaDB-generated MyLite-supported
child key prefixes and exact unique parent keys. Unsupported actions,
unsupported key shapes, volatile/temporary tables, and `DROP FOREIGN KEY` fail
before durable publication. Full InnoDB features, including cascades and
dump-import `foreign_key_checks=0` bypass behavior, remain planned.

## Design

The publication slice is implemented with the following behavior.

1. Extract parsed FK definitions from `HA_CREATE_INFO::alter_info` in
   `ha_mylite::create()`.
   - Ignore old FK entries during copy ALTER.
   - Treat MariaDB-generated supporting keys as ordinary secondary keys when
     their physical key shape is otherwise MyLite-supported.
   - Map MariaDB action and match enums to MyLite storage action and match
     values.

2. Validate every new FK before storing any FK metadata.
   - Reject temporary and volatile zero-file tables.
   - Reject `CASCADE`, `SET NULL`, and `SET DEFAULT` in the first public
     subset.
   - Require parent and child schemas to belong to the same primary `.mylite`
     file.
   - Require child columns to match an explicit or MariaDB-generated supported
     current-table key prefix.
   - Require referenced columns to match an exact supported parent-table unique
     key that can be enforced by MyLite key-prefix lookups.
   - Require compatible nullability metadata so child/parent key marker
     normalization remains sound.

3. Validate the referenced table through catalog-backed metadata, not through
   InnoDB dictionary state.
   - For a self-referencing FK, the current `TABLE` object can supply both
     sides.
   - For an existing parent table, decode the stored table-definition image
     into a temporary `TABLE_SHARE` or equivalent first-party key metadata, then
     inspect parent fields and keys without opening a second handler object for
     DML enforcement.
   - Record the referenced key name chosen by validation so later parent
     update/delete checks can bind to the intended parent key.

4. Publish table and FK metadata atomically.
   - Store the MariaDB table-definition image and all validated FK metadata in
     the same statement checkpoint window.
   - If any FK fails validation or storage publication, the table definition,
     rows copied during ALTER, indexes, autoincrement state, and FK records
     must roll back to the statement-start view.
   - Preserve existing copy-ALTER backup rename behavior so retained FK records
     follow the final logical table and do not follow MariaDB's internal
     `#sql-backup-*` name.

5. Open the public SQL surface through a narrow handler-backed policy.
   - Replace the blanket `libmylite` FK SQL rejection with a narrower policy
     that allows MariaDB to execute supported `CREATE TABLE` and copy
     `ALTER TABLE ... ADD FOREIGN KEY` forms.
   - Continue to reject `CREATE TEMPORARY TABLE` FK DDL and
     `ALTER TABLE ... DROP FOREIGN KEY` at the `libmylite` boundary.
   - Continue to reject unsupported FK actions, volatile FK tables, partitions,
     and unsupported key shapes before durable publication.
   - Keep `HTON_SUPPORTS_FOREIGN_KEYS` disabled until the SQL-layer side
     effects of advertising it have been reviewed separately.

## File Lifecycle

No new durable companion file is introduced. FK records remain catalog records
and typed FK blob pages in the primary `.mylite` file. Failed FK DDL must leave
no partial FK record and no persistent MariaDB `.frm`, `.ibd`, `.MYD`, `.MYI`,
`.MAI`, `.MAD`, `aria_log.*`, binlog, relay-log, or plugin-owned durable
sidecar.

## Embedded Lifecycle And API

No new public C API is required. Direct and prepared execution expose the same
successful DDL, metadata, and DML integrity behavior for the covered subset.
Unsupported FK DDL continues to return an ordinary MyLite or MariaDB diagnostic
through existing error APIs.

## Build, Size, And Dependencies

No new dependency is needed. The likely fork delta is limited to
`mariadb/storage/mylite/`, the existing `libmylite` SQL-surface policy, tests,
and docs. Binary-size impact should be small unless parent-table validation
requires pulling in broader table-open machinery; that choice must be measured
before acceptance.

## Test Plan

- Storage-smoke coverage for supported public FK DDL:
  - `CREATE TABLE child (..., CONSTRAINT ... FOREIGN KEY ...) ENGINE=InnoDB`;
  - `ALTER TABLE child ADD CONSTRAINT ... FOREIGN KEY ...`;
  - close/reopen `SHOW CREATE TABLE` and information-schema metadata;
  - child insert/update missing-parent rejection;
  - parent update/delete referenced-row rejection;
  - failed FK DDL rollback and sidecar gates.
- Unsupported coverage for cascades, `SET NULL`, temporary/volatile tables,
  partitioned tables, unsupported index classes, missing parents, missing
  referenced keys, incompatible column counts, and invalid referenced columns.
- Compatibility harness coverage under the existing `foreign-key` and
  `routed-ddl-dml` groups.
- Verification: storage-smoke build/tests, default storage tests, format check,
  and `git diff --check`.

## Acceptance Criteria

- Public FK DDL is accepted only for the documented supported subset.
- Accepted constraints are durable in one `.mylite` file, visible after
  close/reopen, and enforced by child/parent DML checks.
- Unsupported FK DDL fails before table/FK publication.
- Failed FK DDL restores the statement-start catalog and row/index visibility.
- Docs and compatibility status distinguish the supported MyLite subset from
  full InnoDB FK behavior.

## Risks And Open Questions

- Decoding a parent table-definition image into a temporary `TABLE_SHARE` is
  the current smallest validation bridge. It still needs broader charset,
  collation, and size-impact evidence before the subset is widened.
- Advertising `HTON_SUPPORTS_FOREIGN_KEYS` may change SQL-layer ALTER, DROP,
  TRUNCATE, and prelocking behavior; it should stay a separate review point.
- `foreign_key_checks=0` is common in dumps and still needs explicit import
  semantics before MyLite can claim broad FK compatibility.
- `DROP FOREIGN KEY` and generated-key cleanup semantics remain a specific
  follow-up before MyLite can claim a full FK metadata lifecycle.
