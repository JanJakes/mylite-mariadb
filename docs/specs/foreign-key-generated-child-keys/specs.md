# Foreign-Key Generated Child Keys

## Goal

Accept the MariaDB-generated child supporting indexes that are created for
validated public MyLite foreign-key DDL when the user does not declare an
explicit child key.

This widens the current `CREATE TABLE` and copy `ALTER TABLE ... ADD FOREIGN
KEY` subset without changing the supported FK actions: only immediate
`RESTRICT` / `NO ACTION` constraints over durable MyLite-routed tables and exact
unique parent keys are in scope.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_lex.cc:LEX::add_table_foreign_key()` appends the parsed
  `Foreign_key` entry followed by the current `last_key`, which is the
  auto-generated supporting key for the child columns.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table()` skips
  `Key::FOREIGN_KEY` entries when constructing `KEY` metadata, folds duplicate
  generated FK keys into existing explicit compatible keys, and marks retained
  generated keys with `HA_GENERATED_KEY`.
- `mariadb/sql/sql_class.cc:is_foreign_key_prefix()` treats a generated FK key
  as redundant when it is a prefix of a compatible explicit key.
- Before this slice,
  `mariadb/storage/mylite/ha_mylite.cc:mylite_key_is_supported()` rejected
  `HA_GENERATED_KEY`, so otherwise valid FK DDL without an explicit child index
  failed before MyLite catalog publication.
- `mariadb/include/my_base.h` groups `HA_GENERATED_KEY` with other key flags,
  while `HA_UNIQUE_HASH` remains the separate hidden long-unique hash surface
  that MyLite must continue to reject.

## Compatibility Impact

Common MySQL/MariaDB and ORM DDL frequently relies on the engine-created child
supporting index for foreign keys. Supporting generated child keys makes the
current public FK subset closer to application expectations without claiming
full InnoDB compatibility.

Generated FK child keys remain ordinary MyLite secondary indexes for storage
purposes. Hidden long-unique hash keys, MySQL-style expression indexes,
generated primary keys, cascades, `SET NULL`, `SET DEFAULT`, `DROP FOREIGN
KEY`, `foreign_key_checks=0` import bypass behavior, and full
`HTON_SUPPORTS_FOREIGN_KEYS` advertising remain out of scope.

## Design

Allow `HA_GENERATED_KEY` only when the rest of the key shape is supported by
MyLite:

1. Keep rejecting unsupported algorithms, FULLTEXT, SPATIAL, `HA_UNIQUE_HASH`,
   vector indexes, and unbounded BLOB/TEXT keys.
2. Let generated FK child keys use the same row/index storage as explicit
   secondary keys.
3. Preserve `mylite_find_foreign_key_prefix()` behavior so an explicit child
   key still satisfies FK validation before the generated key when MariaDB kept
   both, while a generated key satisfies validation when no explicit key exists.
4. Keep parent-key validation unchanged: referenced columns must still match an
   exact supported unique parent key.
5. Keep `DROP FOREIGN KEY` unsupported so generated-key cleanup semantics do
   not need to be claimed in this slice.

## File Lifecycle

No new durable companion is introduced. Generated child keys are stored in the
same MariaDB table-definition image and MyLite index-entry pages as other
supported indexes. Failed DDL must leave no table, FK, generated-key metadata,
or durable MariaDB sidecars.

## Tests

- Storage-smoke coverage for `CREATE TABLE ... FOREIGN KEY` without an
  explicit child key, including missing-parent insert rejection, valid insert,
  parent update/delete rejection, `SHOW CREATE TABLE`, and close/reopen
  enforcement.
- Storage-smoke coverage for column-level `REFERENCES` without an explicit
  child key.
- Storage-smoke coverage for `ALTER TABLE ... ADD FOREIGN KEY` without an
  explicit child key.
- Regression coverage that unbounded unique BLOB/TEXT long-hash keys still
  reject before catalog publication.
- Verification: storage-smoke embedded tests, default storage tests, format
  check, and `git diff --check`.

## Acceptance Criteria

- Generated FK child supporting keys are accepted only when the generated key
  shape is otherwise MyLite-supported.
- Accepted constraints are durable, visible through existing FK metadata hooks,
  and enforced after close/reopen.
- Unsupported generated or hidden key classes remain explicit failures.
- Compatibility docs distinguish generated FK child-key support from full
  foreign-key and hidden-index compatibility.
