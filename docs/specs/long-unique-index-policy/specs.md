# Long Unique Index Policy

## Problem

MyLite supports bounded BLOB/TEXT prefix indexes, including generated BLOB/TEXT
prefix indexes, but MariaDB also accepts unbounded `UNIQUE` BLOB/TEXT key parts
by synthesizing hidden long-unique hash metadata. MyLite documentation grouped
that with expression/hidden generated indexes as planned work. The executable
policy should be explicit: MyLite rejects those hidden generated long-unique
hash indexes before catalog publication until the storage layer has a durable
design for them.

MariaDB 11.8 does not expose MySQL-style base-table expression index syntax in
the key-part grammar; key parts are column identifiers with optional prefix
lengths. The relevant hidden generated key surface for routed base tables is
therefore long-unique hash handling, not direct expression-index DDL.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy`: `key_part` accepts `key_part_simple` or
  `ident '(' NUM ')'`; `key_part_simple` is only `ident`. There is no
  base-table `INDEX ((expr))` grammar in this baseline.
- `mariadb/sql/sql_table.cc`: `init_key_info()` marks overlength unique keys
  as needing `HA_KEY_ALG_LONG_HASH`; table finalization adds an internal
  `DB_ROW_HASH_` virtual field flagged `LONG_UNIQUE_HASH_FIELD`.
- `mariadb/sql/table.cc`: `setup_keyinfo_hash()` rewrites long-hash key
  metadata for SQL-layer use, while `re_setup_keyinfo_hash()` restores the
  engine-facing representation.
- `mariadb/sql/handler.cc`: `handler::ha_check_long_uniques()` checks
  duplicate long-unique keys in the server layer when a table share has
  long-unique metadata.
- `mariadb/storage/mylite/ha_mylite.cc`: `mylite_key_is_supported()` rejects
  non-BTREE/undefined algorithms, `HA_UNIQUE_HASH`, and unbounded BLOB key
  parts. Long-unique hash indexes therefore fail the MyLite key-shape gate
  before catalog publication, while generated FK supporting keys are handled
  separately.
- Upstream MTR coverage such as `mariadb/mysql-test/main/long_unique.test`
  exercises `CREATE TABLE t1(a blob unique)` and checks that the synthetic
  `db_row_hash_*` column is hidden from ordinary SQL.

## Scope

- Initial routed `CREATE TABLE` rejection for unbounded unique BLOB/TEXT key
  parts on ordinary columns.
- Initial routed `CREATE TABLE` rejection for unbounded unique BLOB/TEXT key
  parts on generated columns.
- Copy-rebuild standalone/ALTER rejection for adding unbounded unique BLOB/TEXT
  keys to existing ordinary and generated columns.
- Catalog preservation, row visibility, and sidecar checks around those failed
  paths.

## Non-Goals

- Implementing MariaDB long-unique hash storage.
- Implementing MySQL-style expression indexes.
- Supporting hidden generated keys beyond generated FK supporting keys.
- Supporting full or oversized non-unique BLOB/TEXT key payloads.
- Changing generated-column prefix index support, which remains covered for
  bounded key parts.

## Design

No handler change is required for this slice. MyLite should keep using
`mylite_key_is_supported()` as the table-publication gate. The tests make the
policy visible by proving that unsupported long-unique key shapes:

1. fail initial DDL without publishing catalog metadata;
2. fail standalone/copy-rebuild index DDL without adding the index;
3. preserve existing rows after failed rebuild attempts;
4. leave no durable MariaDB sidecars.

## Compatibility Impact

Unbounded unique BLOB/TEXT indexes are explicitly unsupported in the current
MyLite storage profile. Bounded BLOB/TEXT prefix indexes remain supported.
MariaDB expression-index syntax is not a current user-facing compatibility
target because this MariaDB baseline does not parse base-table expression key
parts.

## DDL Metadata Routing Impact

Rejected initial DDL must not create a catalog table. Rejected copy-rebuild
index DDL must leave the existing catalog record and table-definition metadata
unchanged, with no newly visible index name.

## Single-File And Embedded-Lifecycle Impact

No new page type, file-format version, or companion file is introduced. Failed
long-unique DDL must not create durable `.frm`, engine, or hash sidecars.

## Public API, Size, And Dependency Impact

No public API, binary profile, dependency, or license change is expected.

## Test And Verification Plan

- Extend storage-engine smoke coverage for rejected initial unbounded
  `UNIQUE` TEXT DDL.
- Cover the same rejection when the key part is a generated TEXT column.
- Cover rejected standalone/copy-rebuild unbounded unique index additions over
  ordinary and generated BLOB/TEXT columns.
- Verify catalog counts, absent index names, row visibility, close/reopen, and
  sidecar gates.
- Run generated-column, unsupported-index, routed DDL/DML, sidecar, format,
  tidy, preset, and diff checks.

## Acceptance Criteria

- Unbounded unique BLOB/TEXT indexes fail before MyLite catalog publication.
- Failed standalone/copy-rebuild additions do not add indexes or hide rows.
- Docs distinguish unsupported long-unique hidden hash indexes from supported
  bounded BLOB/TEXT prefix indexes.
- Roadmap and compatibility docs no longer imply that MariaDB expression-index
  syntax exists in the selected baseline.

## Risks And Open Questions

- MariaDB long-unique duplicate semantics live partly in the SQL-layer
  `handler::ha_check_long_uniques()` path. Supporting them later needs a
  source-backed storage design for hidden hash key metadata and row-update
  maintenance.
- MySQL expression-index compatibility may become relevant for applications
  that require MySQL 8 syntax. That is a future compatibility decision rather
  than a MariaDB 11.8 storage-engine gap.
