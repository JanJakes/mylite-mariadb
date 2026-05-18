# Generated Column Indexes

## Problem

MyLite supports basic unindexed virtual and stored generated columns, and it
supports ordinary primary, unique, and secondary indexes. Before this slice, the
handler rejected key parts backed by generated fields, so common MariaDB/MySQL
schema patterns such as indexing a normalized generated value remained
unsupported even when the generated expression and key image fit MyLite's
existing row/index format.

The next bounded step is to support generated-column indexes where MariaDB
computes the generated value and MyLite stores ordinary key tuples produced by
MariaDB's key machinery.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc`: table creation checks
  `HA_CAN_VIRTUAL_COLUMNS` before accepting generated columns for an engine,
  and copy `ALTER` rebuilds call `TABLE::update_virtual_fields()` while
  copying rows.
- `mariadb/sql/sql_insert.cc` and `mariadb/sql/sql_update.cc`: insert, replace,
  and update paths call `TABLE::update_virtual_fields(...,
  VCOL_UPDATE_FOR_WRITE)` before handler writes when generated values must be
  populated.
- `mariadb/sql/handler.cc`: handler read wrappers call
  `TABLE::update_virtual_fields(..., VCOL_UPDATE_FOR_READ)` after successful
  row reads when virtual fields are needed.
- `mariadb/sql/key.cc`: `key_copy()` builds index key tuples from a MariaDB row
  buffer. MyLite already uses this function for all supported index entries.
- `mariadb/sql/table.cc`: `TABLE::update_keypart_vcol_info()` attaches
  generated-column metadata to key parts after table metadata is restored.
- `mariadb/storage/mylite/ha_mylite.cc`: before this slice, MyLite advertised
  `HA_CAN_VIRTUAL_COLUMNS`, serialized stored generated values in normal row
  payloads, restored base row buffers for virtual-value computation, and
  rejected generated-field key parts in `mylite_key_is_supported()`.
- Official MariaDB generated-column documentation states that indexes on both
  virtual and persistent generated columns are supported and are considered by
  the optimizer like indexes on ordinary columns:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/generated-columns>.

## Compatibility Impact

Generated columns remain partial, but the supported subset expands from
unindexed generated columns to ordinary secondary and unique indexes on scalar
virtual or stored generated columns. The follow-up generated-column-index DDL
slice extends the same key support to copy-rebuild add, drop, rename, and
standalone index DDL, and the generated unique constraint follow-up covers
named `ADD CONSTRAINT ... UNIQUE` syntax over generated columns. MyLite still
does not claim expression indexes, generated primary-key support beyond
MariaDB's inherited rejection, full or oversized
generated BLOB/TEXT key payloads, foreign-key interactions, or broad
SQL-mode-sensitive expression coverage. A follow-up generated BLOB/TEXT prefix
slice covers bounded generated prefix key payloads declared in initial DDL, and
a later DDL slice covers standalone copy-rebuild
generated prefix index creation/drop.

## Design

Remove the handler key-shape rejection for `key_part->field->vcol_info` while
keeping the other existing gates:

- unsupported key algorithms still reject,
- FULLTEXT, SPATIAL, generated hidden keys, hash keys, and oversized key images
  still reject,
- unbounded BLOB/TEXT key parts still reject.

For supported generated-column key parts, MyLite uses the same path as ordinary
indexes:

1. MariaDB computes generated values before `write_row()` / `update_row()`.
2. MyLite calls `key_copy()` over the current row buffer.
3. Durable index-entry pages store the generated key tuple bytes.
4. MyLite duplicate-key checks compare generated key tuples with `key_buf_cmp()`.
5. Reads through generated indexes return row payloads; MariaDB recomputes
   non-stored virtual fields after the handler read when needed.

## Non-Goals

- Generated primary keys, which MariaDB rejects before handler publication.
- Expression indexes backed by hidden generated keys.
- FULLTEXT, SPATIAL, hash, vector, or long-hash indexes.
- Full or oversized generated BLOB/TEXT key payloads.
- SQL-mode-sensitive expression matrix coverage.
- Transaction rollback, savepoint rollback, or crash recovery beyond existing
  statement-checkpoint and append-only index behavior.

## Single-File And Embedded-Lifecycle Impact

Generated index entries use the existing durable MyLite index-entry pages inside
the primary `.mylite` file. No new page type, sidecar, or file-format version is
introduced.

## Public API Impact

No public `libmylite` API changes are needed. The behavior is exposed through
normal SQL execution and prepared statements inherit the same MariaDB execution
path.

## Storage-Engine Routing Impact

The support applies to routed MyLite tables, including requested
`ENGINE=InnoDB` tables whose effective engine is `MYLITE`. Requested-engine
metadata remains unchanged.

## Build, Size, And Dependencies

No dependency or embedded profile changes are expected. The implementation is a
small handler capability refinement plus tests and docs.

## Test And Verification Plan

- Extend storage-engine smoke coverage for a routed `ENGINE=InnoDB` table with
  virtual and stored generated columns indexed by ordinary secondary and unique
  keys.
- Verify forced-index reads on virtual and stored generated columns.
- Verify duplicate rejection through a unique generated-column index.
- Verify update/delete maintenance for generated index entries.
- Verify generated-index reads after close/reopen.
- Keep FULLTEXT, SPATIAL, MySQL-style expression, hidden generated, and
  unbounded BLOB/TEXT index classes rejected.
- Run the generated-column and unsupported-index compatibility groups plus the
  normal format, tidy, preset, and diff checks.

## Acceptance Criteria

- Supported generated-column indexes are accepted before catalog publication.
- Forced-index reads over virtual and stored generated columns work before and
  after close/reopen.
- Unique generated-column indexes reject duplicates and allow reuse after the
  old row becomes non-live.
- Existing unsupported index classes still reject before catalog publication.
- Compatibility docs and roadmap distinguish generated-column index support from
  broader expression-index work and generated-primary-key rejection.

## Risks And Open Questions

- MariaDB permits many generated expressions. This slice covers deterministic
  scalar expressions that fit existing key-size and row-format limits.
- Generated-column indexes over BLOB/TEXT payloads use the same bounded prefix
  policy as ordinary BLOB/TEXT prefix indexes in follow-up initial-DDL and
  standalone copy-rebuild DDL coverage.
- Future transaction work must make generated index-entry maintenance
  transaction-aware instead of relying on append-only row-state filtering.
