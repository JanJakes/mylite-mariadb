# Foreign-Key Generated-Key Cleanup

## Goal

Complete the generated foreign-key child supporting-key lifecycle for the
current public FK subset.

MyLite should follow MariaDB's copy-ALTER table-definition result for generated
FK child keys:

- when a retained generated child key becomes redundant because an explicit
  compatible child key is added, the generated key should disappear from the
  rebuilt table definition and MyLite index metadata;
- when a FK is dropped, its generated child key should remain as an ordinary
  index unless the user explicitly drops that index;
- after a generated key is no longer needed for retained FK metadata, explicit
  `DROP INDEX` should remove it through the ordinary MyLite index DDL path.

## Non-Goals

- Changing MariaDB's retained-index behavior for `ALTER TABLE ... DROP FOREIGN
  KEY`.
- Cascades, `SET NULL`, `SET DEFAULT`, deferrable constraints, or full InnoDB
  FK behavior.
- Advertising `HTON_SUPPORTS_FOREIGN_KEYS`.
- Free-space reclamation for orphaned index-entry pages after copy rebuilds or
  index drops.
- Durable DDL inside active MyLite transactions.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_lex.cc:LEX::add_table_foreign_key()` appends the parsed
  `Foreign_key` entry followed by its auto-generated child `Key`.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table()` skips
  `Key::FOREIGN_KEY` entries when constructing `KEY` metadata and marks
  retained generated child keys with `HA_GENERATED_KEY`.
- `mariadb/sql/sql_class.cc:is_foreign_key_prefix()` treats a generated FK
  child key as redundant when it is a prefix of another compatible generated or
  explicit key.
- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` rebuilds the
  `Alter_info` key list for copy ALTER from retained table keys, preserving
  `HA_GENERATED_KEY`, and then runs the same create-table key preparation that
  can remove redundant generated keys.
- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` clears
  `HA_GENERATED_KEY` when the user explicitly renames a key, so the renamed key
  is no longer eligible for automatic generated-key cleanup.
- `mariadb/sql/sql_table.cc:fk_prepare_copy_alter_table()` removes FKs named
  in `ALTER TABLE ... DROP FOREIGN KEY` from FK compatibility checks, but it
  does not itself add a matching `DROP INDEX`.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_key_is_supported()` accepts
  `HA_GENERATED_KEY` when the rest of the key shape is supported, while
  `mylite_validate_foreign_key_shape()` still rejects dropping the last
  supporting key for retained FK metadata through MariaDB's copy-ALTER checks.

## Compatibility Impact

Generated child-key cleanup moves from planned to covered for the common
MariaDB-compatible paths that matter after generated FK key support:

- explicit supporting key additions can replace MariaDB-generated child keys,
- `DROP FOREIGN KEY` removes FK metadata but keeps retained child index
  metadata visible,
- explicit `DROP INDEX` can remove a generated child index once no retained FK
  depends on it.

This does not claim full InnoDB FK lifecycle parity. Unsupported actions,
hidden key classes, transactional DDL, and free-space reclamation remain
separate work.

## Design

1. Keep the handler using MariaDB's rebuilt table definition as the authority
   for live key metadata after copy ALTER.
2. Do not add MyLite-specific automatic index removal on `DROP FOREIGN KEY`;
   MariaDB's table-definition rebuild keeps that index unless ordinary key
   duplicate cleanup or an explicit `DROP INDEX` removes it.
3. Verify that a compatible explicit child key added to a table with a
   generated FK key causes MariaDB to remove the generated key from the rebuilt
   definition and MyLite to publish only the retained explicit key metadata.
4. Verify that retained FK row checks still work after generated-key cleanup.
5. Verify that explicit `DROP INDEX` after `DROP FOREIGN KEY` removes the
   formerly generated child key through the ordinary index-DLL path.

## File Lifecycle

No new companion files are introduced. Copy ALTER and index drops continue to
publish rebuilt table definitions and live index metadata through the primary
`.mylite` file, with rollback journals and statement checkpoints protecting the
existing publication paths. Old index-entry pages remain orphaned until the
future free-space/compaction work.

## Embedded Lifecycle And API

The behavior is visible through ordinary direct and prepared SQL execution,
`SHOW CREATE TABLE`, `INFORMATION_SCHEMA.STATISTICS`,
`INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`, forced-index reads, row checks,
and close/reopen.

## Build, Size, And Dependencies

No dependency or size-profile change is expected.

## Test Plan

- Storage-smoke coverage for adding an explicit compatible child key to a table
  whose FK originally used a MariaDB-generated child key:
  - generated key is visible before cleanup,
  - explicit key is visible after cleanup,
  - generated key is absent after cleanup,
  - FK metadata and child/parent row checks still work,
  - close/reopen preserves the cleaned-up key set and FK behavior.
- Storage-smoke coverage for dropping a FK that used a generated child key:
  - FK metadata disappears,
  - generated child key remains visible as an ordinary index,
  - explicit `DROP INDEX` removes the retained key,
  - row DML remains unconstrained by the dropped FK after close/reopen.
- Storage-smoke coverage that dropping a generated child key still fails while
  a retained FK depends on it.
- Verification: storage-smoke embedded tests, default storage tests,
  format-check, and `git diff --check`.

## Acceptance Criteria

- Redundant generated FK child keys are removed when MariaDB's copy-ALTER
  rebuilt definition replaces them with explicit compatible keys.
- `DROP FOREIGN KEY` keeps the generated child key unless the SQL also removes
  or later drops that index through a supported path.
- MyLite FK metadata, index metadata, row checks, and close/reopen behavior
  agree with the rebuilt MariaDB table definition.
- Docs and compatibility matrix distinguish generated-key cleanup from full FK
  lifecycle support.
