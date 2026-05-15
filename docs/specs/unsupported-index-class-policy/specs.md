# Unsupported Index Class Policy

## Goal

Make unsupported MyLite index classes fail explicitly before catalog
publication, and track that behavior as compatibility evidence.

This slice covers initial `CREATE TABLE` rejection for FULLTEXT and SPATIAL
index definitions on routed `ENGINE=InnoDB` tables. Generated-column secondary
and unique indexes are covered by the later generated-column-index slice,
generated primary keys inherit MariaDB's SQL-layer rejection, and hidden
long-unique hash indexes are covered by the later long-unique policy slice.

## Non-Goals

- Implement FULLTEXT or SPATIAL access paths.
- Implement MySQL-style expression indexes, hidden generated, hash, vector, or
  oversized multi-page index keys. Generated primary keys follow MariaDB's
  SQL-layer rejection.
- Add broad optimizer or MTR comparison coverage.
- Change supported primary, unique, secondary, or bounded BLOB/TEXT prefix
  index behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy`: parses ordinary, `FULLTEXT`, and `SPATIAL` key
  definitions in `CREATE TABLE` and index DDL.
- `mariadb/sql/sql_table.cc`: prepares table and key definitions before
  calling the storage engine `create()` method.
- `mariadb/storage/mylite/ha_mylite.cc`: `ha_mylite::create()` now applies the
  same key-shape gate used by copy `ALTER`, and `mylite_key_is_supported()`
  rejects FULLTEXT, SPATIAL, hidden generated, hash, unsupported algorithm,
  oversized, and unbounded BLOB/TEXT key shapes.
- MariaDB documentation describes FULLTEXT and SPATIAL indexes as index classes
  with storage-engine-specific implementation requirements:
  <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/full-text-indexes/full-text-index-overview>
  and
  <https://mariadb.com/docs/server/reference/sql-structure/geometry/spatial-index>.

## Compatibility Impact

MyLite supports ordinary primary, unique, and secondary key shapes that can be
represented by the current durable index-entry pages. Index classes with
engine-specific semantics need separate storage designs. Until then, failing
before catalog publication is the compatibility contract: applications receive
a stable DDL failure instead of a table that appears to support an index class
MyLite cannot maintain.

## Design

Use the existing MyLite key-shape gate as the storage-engine boundary:

- supported key shapes continue to publish catalog metadata,
- unsupported key shapes reject from `ha_mylite::create()` before the MyLite
  catalog is updated,
- failed creates leave no table catalog record and no durable sidecars.

Generated-column indexes were expanded by the generated-column-index slice;
this slice keeps explicit FULLTEXT and SPATIAL create-time coverage and groups
the unsupported-index policy in the compatibility harness.

## File Lifecycle

Unsupported index-class DDL must not publish a MyLite table-definition record,
row pages, index pages, or durable MariaDB sidecars. Existing sidecar gates and
catalog-count assertions are the lifecycle proof.

## Embedded Lifecycle And API

`mylite_exec()` reports a normal SQL failure through MariaDB diagnostics and a
MyLite error result. No public API changes are needed.

## Build, Size, And Dependencies

No new dependencies, no format change, and no meaningful binary-size impact.

## Test Plan

- Storage-engine smoke rejects `CREATE TABLE` with a FULLTEXT index.
- Storage-engine smoke rejects `CREATE TABLE` with a SPATIAL index.
- Each failed DDL verifies no MyLite catalog record exists and catalog table
  counts remain unchanged.
- Add a compatibility harness group for unsupported index classes.
- Run formatting, tidy, configured CTest presets, the named harness report, and
  `git diff --check`.

## Acceptance Criteria

- FULLTEXT and SPATIAL indexes reject before catalog publication on routed
  tables.
- Supported ordinary indexes remain covered by existing storage smoke tests.
- Compatibility docs and roadmap describe unsupported index-class rejection as
  partial compatibility policy, while generated-column index support is tracked
  separately.
- The compatibility harness can run the unsupported-index evidence by name.

## Risks And Open Questions

- Standalone `CREATE INDEX` coverage for unsupported classes remains narrower
  than initial `CREATE TABLE` coverage.
- FULLTEXT, SPATIAL, MySQL-style expression, hidden generated, hash, vector,
  and oversized index support each need explicit physical design, recovery
  behavior, and optimizer coverage before support can be claimed. Generated
  primary keys remain governed by MariaDB's SQL-layer rejection.
