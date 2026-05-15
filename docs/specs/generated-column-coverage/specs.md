# Generated Column Coverage

## Goal

Cover basic MariaDB generated columns for MyLite-routed tables where MariaDB
can compute generated values through its existing virtual-column machinery and
MyLite can persist the MariaDB table-definition image plus stored row buffers in
the `.mylite` file.

This moved generated columns from planned to partial support for `VIRTUAL` and
`STORED` columns under basic `CREATE TABLE`, insert, update, and close/reopen
behavior. A later generated-column-index slice expands the covered subset to
ordinary secondary and unique indexes on scalar generated columns.

## Non-Goals

- Implement a MyLite-native generated-column expression evaluator.
- Support foreign keys, FULLTEXT, SPATIAL, expression indexes, or generated
  primary keys.
- Add broad expression, `ALTER`, CTAS, dump-import, prepared-statement, or MTR
  coverage.
- Implement SQL rollback for generated-column writes.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/unireg.cc`: `pack_vcols()` stores virtual and stored generated
  column expressions in the table-definition image.
- `mariadb/sql/table.cc`: `parse_vcol_defs()` restores generated-column
  expressions from the table-definition image, and
  `TABLE::update_virtual_fields()` computes generated values.
- `mariadb/sql/handler.cc`: handler read wrappers call
  `TABLE::update_virtual_fields(..., VCOL_UPDATE_FOR_READ)` after successful
  row reads when the statement reads virtual fields.
- `mariadb/sql/sql_insert.cc` and `mariadb/sql/sql_update.cc`: write paths call
  `TABLE::update_virtual_fields(..., VCOL_UPDATE_FOR_WRITE)` before handler
  writes when generated fields must be populated.
- `mariadb/sql/sql_table.cc`: table creation checks
  `HA_CAN_VIRTUAL_COLUMNS` before allowing generated columns for a storage
  engine, and copy `ALTER` updates virtual fields while rebuilding rows.
- `mariadb/sql/field.h` and `mariadb/sql/table.cc`: non-stored generated
  columns are not stored in the storage record, while stored generated columns
  are present in the row buffer after generated-value calculation.
- MariaDB documentation describes generated-column syntax, virtual vs.
  persistent/stored behavior, DML behavior, index support, and expression
  limitations:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/generated-columns>.

## Compatibility Impact

Generated columns are SQL-layer expression semantics plus storage-engine
capability flags. MyLite supports basic generated columns by advertising
`HA_CAN_VIRTUAL_COLUMNS`, preserving MariaDB table-definition metadata, storing
generated persistent values as part of the row buffer, and letting MariaDB
compute non-stored virtual values after reads.

The follow-up generated-column-index slice reuses MariaDB key tuple generation
for ordinary secondary and unique indexes on scalar virtual or stored generated
columns. Expression/hidden generated indexes and generated primary keys remain
unsupported so the compatibility matrix does not overstate generated/expression
index support.

## Design

- Add `HA_CAN_VIRTUAL_COLUMNS` to the MyLite handler table flags.
- Apply the existing key-shape support gate to initial `CREATE TABLE` as well
  as copy `ALTER` paths.
- Add storage-engine smoke coverage for a routed `ENGINE=InnoDB` table with:
  - one ordinary primary key,
  - one ordinary base column,
  - virtual generated columns,
  - one stored generated column,
  - ordinary secondary and unique generated-column indexes.
- Verify insert, update, full-scan read, close/reopen persistence, and
  generated-index reads and duplicate checks.

## File Lifecycle

Generated-column metadata is stored inside the MariaDB table-definition image
already held in the MyLite catalog. Stored generated values live in normal row
payloads. Virtual generated values are computed from restored base row buffers.
No new files, companions, or sidecars are introduced.

## Embedded Lifecycle And API

`mylite_exec()` exposes generated-column behavior through normal MariaDB SQL
execution and diagnostics. The public API does not gain new entry points.
Prepared statements inherit the same MariaDB execution path, but this slice
only adds direct-SQL storage-smoke coverage.

## Build, Size, And Dependencies

No new dependencies and no file-format version change. The handler advertises
one additional MariaDB capability flag; measured storage-smoke archive size is
unchanged for practical purposes.

## Test Plan

- Storage-engine smoke creates a routed table with virtual and stored generated
  columns.
- Storage-engine smoke verifies generated values after insert and update.
- Storage-engine smoke verifies generated values after close/reopen.
- Storage-engine smoke verifies ordinary generated-column indexes for
  forced-index reads, duplicate checks, update maintenance, and close/reopen.
- Add a compatibility harness group for generated columns.
- Run formatting, tidy, configured CTest presets, the named harness report, and
  `git diff --check`.

## Acceptance Criteria

- Basic unindexed virtual generated columns are computed from MyLite-restored
  base row buffers before and after close/reopen.
- Basic unindexed stored generated columns persist in MyLite row payloads before
  and after close/reopen.
- Ordinary generated-column secondary and unique indexes work before and after
  close/reopen.
- Compatibility docs and roadmap mark generated columns as partial rather than
  planned.
- The compatibility harness can run the generated-column evidence by name.

## Risks And Open Questions

- Broader expression classes, SQL-mode-sensitive expressions, generated
  columns with BLOB/TEXT payloads, `ALTER TABLE ADD/DROP/MODIFY` generated
  columns, CTAS, dump/import, prepared diagnostics, and rollback remain
  uncovered.
- Generated primary keys, expression/hidden generated indexes, generated
  BLOB/TEXT key payloads, and broader expression matrices need separate specs
  before support is claimed.
