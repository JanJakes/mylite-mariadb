# Generated Column ALTER DDL

## Problem

MyLite supports generated columns declared in initial `CREATE TABLE`, and it now
supports ordinary generated-column indexes. The remaining generated-column DDL
gap is copy-rebuild `ALTER TABLE` for adding, modifying, and dropping generated
columns after rows already exist.

MariaDB owns generated-expression parsing and evaluation. MyLite should prove
that catalog-backed table-definition metadata, row copying, stored generated
values, virtual generated reads, and close/reopen discovery all survive the
supported copy ALTER lifecycle.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc`: copy `ALTER` rebuilds create a new table
  definition, copy rows, and call `TABLE::update_virtual_fields()` when virtual
  fields must be populated during row copy.
- `mariadb/sql/unireg.cc`: generated-column expressions are packed into the
  table-definition image.
- `mariadb/sql/table.cc`: generated-column expressions are unpacked from the
  table-definition image on reopen, and `TABLE::update_virtual_fields()`
  recomputes generated values on read/write paths.
- `mariadb/storage/mylite/ha_mylite.cc`: MyLite copy rebuilds publish the
  rebuilt table definition and copied rows through the existing catalog and row
  append path.

## Compatibility Impact

Generated-column support expands to copy-rebuild ALTER DDL for:

- adding virtual and stored generated columns to a populated routed table,
- modifying a generated-column expression,
- dropping a generated column,
- preserving the resulting metadata and values after close/reopen.

Instant, in-place, online, SQL rollback, CTAS, broad dump/export, broad expression
matrices, and MySQL-style expression-index compatibility remain planned.
Generated primary-key DDL follows MariaDB's SQL-layer rejection policy, and
hidden long-unique hash indexes are covered by a separate unsupported-policy
slice.

## Design

No MyLite expression evaluator is introduced. The supported path is:

1. MariaDB parses and validates generated-column ALTER definitions.
2. MariaDB copy rebuilds the table and computes generated values while copying
   rows.
3. MyLite stores the rebuilt MariaDB table-definition image in the catalog.
4. Stored generated values are included in normal row payloads.
5. Virtual generated values are recomputed by MariaDB after rows are read.

## Non-Goals

- Online, instant, in-place, or lock-free generated-column ALTER.
- Generated-column target definitions in CTAS or broader dump/export fixtures.
- Exhaustive generated expression compatibility.
- Generated BLOB/TEXT payload matrices.
- Transaction rollback or crash recovery for interrupted ALTER beyond current
  statement-checkpoint behavior.

## Single-File And Embedded-Lifecycle Impact

Successful generated-column ALTER appends rebuilt table definitions and rows to
the primary `.mylite` file through the existing copy-rebuild path. It must not
publish durable MariaDB sidecars or require runtime schema directories after
close/reopen.

## Test And Verification Plan

- Add storage-engine smoke coverage for a routed `ENGINE=InnoDB` table with
  existing rows and no initial generated columns.
- Add virtual and stored generated columns through `ALTER TABLE ... ALGORITHM=COPY`.
- Verify generated values before and after close/reopen.
- Modify the virtual generated expression through copy ALTER and verify the
  new value.
- Drop the stored generated column and verify SQL name resolution fails while
  the remaining generated column still works.
- Reopen again and repeat representative checks.
- Run generated-column, routed DDL/DML, sidecar, format, tidy, preset, and diff
  checks.

## Acceptance Criteria

- Copy ALTER can add virtual and stored generated columns to a populated routed
  table.
- Modified generated expressions are reflected in reads.
- Dropped generated columns disappear from SQL metadata and name resolution.
- The final generated-column metadata survives close/reopen without durable
  sidecars.
- Compatibility docs and roadmap distinguish copy ALTER coverage from online
  ALTER, CTAS, broader dump/export, and broader expression work.

## Risks And Open Questions

- This slice relies on the existing append-only copy-rebuild and statement
  checkpoint model.
- Generated-column ALTER combined with generated indexes, foreign keys,
  partitions, or unsupported expression classes needs separate coverage.
