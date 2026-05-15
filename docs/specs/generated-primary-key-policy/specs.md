# Generated Primary-Key Policy

## Problem

MyLite documentation has treated generated primary keys as future storage work,
but MariaDB 11.8 rejects primary keys based on generated columns before handler
catalog publication. MyLite should track that as an inherited MariaDB
compatibility policy rather than implying a MyLite storage gap.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc`: when building a primary key part, MariaDB checks
  `column->vcol_info` and raises `ER_PRIMARY_KEY_BASED_ON_GENERATED_COLUMN`
  before calling the storage handler.
- `mariadb/sql/share/errmsg-utf8.txt`: the diagnostic text is
  `Primary key cannot be defined upon a generated column`.
- `mariadb/include/mysql.h`: the legacy virtual-column error macro aliases to
  the generated-column primary-key error.

## Scope

- Initial `CREATE TABLE` rejection for a generated-column primary key on a
  MyLite-routed `ENGINE=InnoDB` table.
- Copy-rebuild `ALTER TABLE ... ADD PRIMARY KEY` rejection when the key part is
  a generated column.
- Catalog, row visibility, close/reopen, and sidecar checks around the failed
  generated primary-key DDL.

## Non-Goals

- Implementing generated primary keys despite MariaDB's SQL-layer rejection.
- MySQL-specific divergence from MariaDB generated primary-key behavior.
- Generated foreign-key constraints, expression/hidden indexes, or unsupported
  index classes.
- SQL rollback beyond verifying that failed DDL does not publish MyLite catalog
  metadata for rejected creates and does not lose the source table for rejected
  ALTER.

## Design

No MyLite handler change is needed. MyLite should let MariaDB reject generated
primary-key definitions before `ha_mylite::create()` publishes a table
definition. Tests should prove that:

1. rejected initial DDL leaves no MyLite catalog table;
2. rejected copy ALTER leaves the original table and row visible;
3. close/reopen still discovers only the valid original table;
4. no durable sidecars are produced by either failed path.

## Compatibility Impact

Generated primary keys move from a vague planned item to explicit inherited
MariaDB unsupported behavior. Ordinary generated secondary and unique indexes
remain supported within their documented subset.

## DDL Metadata Routing Impact

Generated primary-key failures must not publish a new catalog record. Failed
ALTER must keep the existing catalog record visible and unchanged.

## Single-File And Embedded-Lifecycle Impact

No new durable data format is introduced. Failed generated primary-key DDL must
not leave durable MariaDB sidecars or require runtime schema directories after
close/reopen.

## Public API, File-Format, Size, And Dependency Impact

No public API, file-format, dependency, or profile change is expected.

## Test And Verification Plan

- Add storage-engine smoke coverage for failed initial generated primary-key
  DDL with the MariaDB diagnostic text.
- Add storage-engine smoke coverage for failed copy `ALTER TABLE ... ADD
  PRIMARY KEY` over a generated column.
- Verify catalog count/table existence, row visibility, close/reopen discovery,
  and sidecar gates after failures.
- Run generated-column, unsupported-index, routed DDL/DML, sidecar, format,
  tidy, preset, and diff checks.

## Acceptance Criteria

- Generated primary-key create and alter attempts fail with MariaDB's generated
  primary-key diagnostic.
- Rejected initial DDL does not create a MyLite catalog table.
- Rejected ALTER preserves the source table and rows before and after
  close/reopen.
- Compatibility docs describe generated primary keys as an inherited MariaDB
  unsupported surface, not as unplanned MyLite behavior.

## Risks And Open Questions

- MySQL compatibility may differ for some generated-column key forms. MyLite's
  current authority is MariaDB 11.8, so any future divergence needs a separate
  compatibility decision.
