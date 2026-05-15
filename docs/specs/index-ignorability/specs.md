# Index Ignorability

## Goal

Cover MariaDB-compatible ignored-index metadata for MyLite-routed tables.
Supported secondary indexes marked `IGNORED` should remain in the MyLite catalog
and row/index storage, but be hidden from optimizer index selection and index
hints until marked `NOT IGNORED` again.

## Non-Goals

- Do not add MySQL `VISIBLE` / `INVISIBLE` index syntax; MariaDB uses
  `IGNORED` / `NOT IGNORED`.
- Do not cover FULLTEXT, SPATIAL, vector, partitioned, primary-key, or
  unsupported index-class ignorability in this slice.
- Do not implement online/in-place ignored-index ALTER. MyLite still supports
  copy-rebuild DDL for the current routed table shapes.
- Do not change MyLite physical index storage. Ignored indexes may still be
  maintained physically; this slice covers SQL metadata and optimizer
  visibility.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation describes ignored indexes as optimizer-hidden indexes,
  allows `IGNORED` in index definitions, allows
  `ALTER TABLE ... ALTER INDEX [IF EXISTS] ... [NOT] IGNORED`, and exposes the
  state through `SHOW INDEX` and `INFORMATION_SCHEMA.STATISTICS`.
  Source: <https://mariadb.com/kb/en/ignored-indexes/>
- `mariadb/sql/sql_yacc.yy:7330-7332` parses `IGNORED` / `NOT IGNORED` in index
  definitions and stores the requested state on `key_create_info.is_ignored`.
- `mariadb/sql/sql_yacc.yy:8101-8110` parses
  `ALTER TABLE ... ALTER INDEX [IF EXISTS] ident [NOT] IGNORED` into
  `alter_index_ignorability_list` and marks the ALTER with
  `ALTER_INDEX_IGNORABILITY`.
- `mariadb/sql/sql_table.cc:3919` copies the create-time ignored flag into the
  resulting `KEY`.
- `mariadb/sql/sql_table.cc:7460-7478` applies existing-index ignorability
  changes to the rebuilt key metadata and records the altered index.
- `mariadb/sql/sql_table.cc:9362-9375` preserves or changes ignored-index state
  during table recreation and rejects primary-key ignorability.
- `mariadb/sql/unireg.cc:123-134` serializes ignored-index flags into the
  binary `.frm` image.
- `mariadb/sql/table.cc:2218-2232` reads ignored-index flags back from the
  binary `.frm` image and initializes `TABLE_SHARE::ignored_indexes`.
- `mariadb/sql/table.cc:1484-1504` computes `ignored_indexes` and removes them
  from optimizer-usable indexes.
- `mariadb/sql/sql_show.cc:7526-7528` exposes the ignored state in the `IGNORED`
  column used by `SHOW INDEX` and `INFORMATION_SCHEMA.STATISTICS`.
- `mariadb/mysql-test/main/ignored_index.test` covers upstream syntax,
  metadata, optimizer hiding, primary-key rejection, and selected algorithm
  behavior.

## Compatibility Impact

This changes MyLite's compatibility status for supported secondary indexes from
uncovered to partial:

- `CREATE TABLE` / `CREATE INDEX` / `ALTER TABLE ADD INDEX` can create
  supported ignored secondary indexes;
- `ALTER TABLE ... ALTER INDEX IF EXISTS ... IGNORED` should mark existing
  supported secondary indexes ignored and skip missing indexes with warnings;
- `ALTER TABLE ... ALTER INDEX IF EXISTS ... NOT IGNORED` should restore
  optimizer visibility and skip missing indexes with warnings;
- ignored indexes should reject index hints as MariaDB does;
- `SHOW INDEX` / `INFORMATION_SCHEMA.STATISTICS` should expose the ignored
  state before and after close/reopen.

The behavior remains partial because unsupported index classes, primary-key
ignorability, partitions, foreign keys, and online/in-place algorithm variants
remain out of scope.

## Proposed Design

No production code change is expected. MyLite already stores MariaDB's binary
table-definition image in the catalog through `ha_mylite::create()` and
discovers it through `mylite_discover_table()`. MariaDB serializes the
ignored-index flag into that image and restores it into `TABLE_SHARE` during
discovery.

The storage-smoke test should prove that this catalog publication works for
MyLite-routed tables by checking metadata, index-hint behavior, and close/reopen
discovery.

## Affected Subsystems

- MariaDB parser and ALTER metadata handling, inherited unchanged.
- MyLite handler catalog publication and discovery through the stored binary
  table definition.
- MyLite compatibility harness routed-DDL group and compatibility matrix.

## DDL Metadata Routing Impact

Supported ignored-index state is metadata in the MariaDB table definition. It
must be routed into the MyLite catalog with the rest of the table definition
and must not require durable MariaDB sidecars.

## Single-File And Lifecycle Impact

Ignored-index state is durable metadata in the `.mylite` file. The slice must
prove no persistent `.frm`, engine, or log sidecars remain after ignored-index
create, alter, close, reopen, and final close.

## Public API And File-Format Impact

No public C API or first-party file-format change is expected. Existing SQL
execution and warning APIs should expose MariaDB errors and warnings from the
ignored-index statements.

## Storage-Engine Routing Impact

Ignored indexes still route to MyLite when their index class is otherwise
supported. MyLite may continue maintaining physical index entries for ignored
indexes; optimizer visibility is controlled by MariaDB table metadata.

## Binary-Size, License, And Dependency Impact

No binary-size-sensitive dependency, license, or build-profile change is
expected.

## Test Plan

1. Add storage-smoke coverage for `CREATE TABLE` with one `IGNORED` and one
   `NOT IGNORED` supported secondary index.
2. Assert `INFORMATION_SCHEMA.STATISTICS.IGNORED` reports `YES` and `NO`.
3. Assert index hints against an ignored index fail, while hints against a
   not-ignored supported index work.
4. Add coverage for `CREATE INDEX ... IGNORED` and
   `ALTER TABLE ... ADD INDEX ... NOT IGNORED` on supported secondary indexes.
5. Add coverage for `ALTER TABLE ... ALTER INDEX IF EXISTS ... IGNORED` and
   `... NOT IGNORED`, including warning-producing skips for missing indexes.
6. Assert ignored/not-ignored state survives close/reopen and sidecar gates.
7. Run format, focused storage-smoke tests, compatibility harness routed-DDL and
   sidecar reports, clang-tidy, and the `dev`, `embedded-dev`, and
   `storage-smoke-dev` gates.

## Acceptance Criteria

- Supported ignored secondary indexes are visible in metadata with
  `IGNORED='YES'`.
- Supported not-ignored secondary indexes are visible in metadata with
  `IGNORED='NO'`.
- Index hints against ignored supported secondary indexes fail with the same
  user-facing shape as missing-index hints.
- `ALTER INDEX IF EXISTS ... IGNORED` hides an existing supported secondary
  index and warns without mutation for a missing index.
- `ALTER INDEX IF EXISTS ... NOT IGNORED` restores a supported secondary index
  and warns without mutation for a missing index.
- Ignored/not-ignored metadata survives close/reopen discovery.
- Durable sidecar gates pass.

## Risks And Unresolved Questions

- Optimizer plan selection can be data-dependent, so the test should prefer
  index-hint failures and metadata assertions over fragile plan-shape checks.
- The test should avoid claiming physical index maintenance behavior beyond the
  existing supported index-storage coverage.
