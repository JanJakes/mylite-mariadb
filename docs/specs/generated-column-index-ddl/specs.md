# Generated Column Index DDL

## Problem

MyLite now accepts ordinary generated-column indexes declared in `CREATE TABLE`.
The adjacent compatibility gap is DDL that adds, drops, or renames those indexes
after table creation. MariaDB routes standalone index DDL and supported
`ALTER TABLE ... ADD/DROP/RENAME INDEX` paths through the same copy-rebuild
machinery MyLite already uses for base-column indexes, so MyLite should prove
generated key parts survive that lifecycle too.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_parse.cc`: standalone `CREATE INDEX` and `DROP INDEX`
  statements are handled by calling `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc`: copy `ALTER` rebuilds call
  `TABLE::update_virtual_fields()` while copying rows, and the rebuilt table
  definition includes the new or renamed key metadata.
- `mariadb/sql/table.cc`: `TABLE::update_keypart_vcol_info()` restores
  generated-column metadata on key parts after table metadata is reopened.
- `mariadb/sql/key.cc`: `key_copy()` constructs key tuples from the active row
  buffer, including generated values already computed by MariaDB.
- `mariadb/storage/mylite/ha_mylite.cc`: MyLite's handler now accepts
  generated-field key parts when the overall key shape remains otherwise
  supported.

## Compatibility Impact

Generated-column indexes remain partial, but the covered subset expands from
initial `CREATE TABLE` declarations to supported copy-rebuild index DDL:

- `ALTER TABLE ... ADD KEY ..., ALGORITHM=COPY`,
- standalone `CREATE INDEX ... ALGORITHM=COPY`,
- `ALTER TABLE ... RENAME INDEX ..., ALGORITHM=COPY`,
- standalone `DROP INDEX`.

Online, instant, in-place, hidden expression indexes, foreign keys, and
transactional DDL rollback remain planned. Generated primary-key DDL follows
MariaDB's SQL-layer rejection policy.

## Design

No new storage format is needed. MyLite relies on MariaDB's copy-rebuild path:

1. MariaDB parses index DDL into `Alter_info`.
2. MariaDB rebuilds the table definition and computes generated values while
   copying rows.
3. MyLite validates the rebuilt key shapes in `ha_mylite::create()`.
4. MyLite appends the rebuilt table definition, rows, and generated key tuples
   to the primary `.mylite` file.
5. MyLite removes intermediate catalog identities through the existing
   copy-rebuild cleanup path.

## Non-Goals

- Online, instant, in-place, or lock-free index DDL.
- Generated primary keys, which MariaDB rejects before handler publication.
- Expression indexes backed by hidden generated columns.
- FULLTEXT, SPATIAL, hash, vector, long-hash, or oversized/full BLOB/TEXT
  indexes.
- SQL transaction rollback or crash recovery for interrupted DDL beyond the
  current statement-checkpoint coverage.

## Single-File And Embedded-Lifecycle Impact

Successful generated-index DDL publishes only MyLite catalog and index-entry
pages in the primary `.mylite` file. It must not introduce durable `.frm`,
InnoDB, MyISAM, Aria, binlog, relay-log, or plugin-owned sidecars.

## Test And Verification Plan

- Extend storage-engine smoke coverage with a routed `ENGINE=InnoDB` table that
  has virtual and stored generated columns but no generated indexes at initial
  creation.
- Add a virtual generated-column unique index through `ALTER TABLE`.
- Add a stored generated-column secondary index through standalone
  `CREATE INDEX`.
- Verify forced-index reads and duplicate checks.
- Rename the generated unique index and verify old-name rejection plus new-name
  lookup.
- Drop the generated secondary index and verify metadata removal plus row
  visibility.
- Reopen the file, verify the renamed generated index, then create and drop a
  generated secondary index through the default standalone path.
- Run the generated-column, routed DDL/DML, sidecar, and unsupported-index
  compatibility evidence plus normal format, tidy, preset, and diff checks.

## Acceptance Criteria

- Generated-column indexes can be added, renamed, and dropped through supported
  copy-rebuild DDL.
- Forced-index reads over generated indexes work before and after close/reopen.
- Duplicate checks on generated unique indexes survive DDL rename.
- Dropped generated indexes disappear from `SHOW INDEX` and `FORCE INDEX` while
  rows remain readable.
- Compatibility docs and roadmap distinguish generated-index DDL support from
  online DDL, generated-primary-key rejection, and expression/hidden generated
  indexes.

## Risks And Open Questions

- Copy-rebuild DDL inherits the current append-only storage and statement
  checkpoint limits.
- Broader generated expression coverage, full or oversized generated BLOB/TEXT
  key payloads, and default/online generated BLOB/TEXT prefix index DDL need
  separate compatibility matrices.
