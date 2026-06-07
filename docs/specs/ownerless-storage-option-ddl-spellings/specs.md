# Ownerless Storage Option DDL Spellings

## Problem Statement

Ownerless table storage-option policy rejects unproven InnoDB native file-layout
options such as page compression, encryption, and table `TABLESPACE`. Existing
coverage proves plain `CREATE TABLE` and `ALTER TABLE` spellings plus quoted
identifier and CTAS false-positive regressions. MariaDB routes additional
`CREATE TABLE` spellings through the same create-table option grammar, including
`IF NOT EXISTS`, `CREATE OR REPLACE`, and `TEMPORARY`.

This slice broadens the policy evidence so those spellings also fail before
MariaDB can enter native file-layout, encryption, or tablespace-placement paths.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:2526` through `mariadb/sql/sql_yacc.yy:2555`
  parses `CREATE [OR REPLACE] [TEMPORARY] TABLE [IF NOT EXISTS]` before the
  table identifier and shared `create_body`.
- `mariadb/sql/sql_yacc.yy:4799` through `mariadb/sql/sql_yacc.yy:4802`
  routes ordinary create bodies through `opt_create_table_options`.
- `mariadb/sql/sql_yacc.yy:5914` through `mariadb/sql/sql_yacc.yy:5955`
  stores engine-defined table options from `ident_options = value` tokens.
- `mariadb/sql/sql_yacc.yy:5892` handles top-level table `TABLESPACE ident`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:660` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:671` registers InnoDB
  `PAGE_COMPRESSED`, `PAGE_COMPRESSION_LEVEL`, `ENCRYPTED`, and
  `ENCRYPTION_KEY_ID` table options.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11383` through
  `mariadb/storage/innobase/handler/ha_innodb.cc:11523` validates those
  options against native InnoDB page compression, encryption, key management,
  file-per-table, row-format, and key-block constraints.
- `packages/libmylite/src/database.cc` already rejects ownerless table storage
  options in `is_unsupported_ownerless_table_storage_option_statement()` before
  executing MariaDB SQL.

## Design

Extend the existing `table-storage-option-policy` selector with representative
additional create-table spellings:

- `CREATE TABLE IF NOT EXISTS ... PAGE_COMPRESSED=1`,
- `CREATE OR REPLACE TABLE ... PAGE_COMPRESSION_LEVEL=3`,
- `CREATE TEMPORARY TABLE ... ENCRYPTED=YES`,
- `CREATE OR REPLACE TEMPORARY TABLE ... ENCRYPTION_KEY_ID=1`,
- `CREATE TABLE IF NOT EXISTS ... TABLESPACE ...`.

The selector already verifies the canonical create-time and alter-time
storage-option rejections, quoted identifier false positives, CTAS alias false
positives, row preservation, metadata absence, and ownerless/native reopen
before and after forced `.shm` rebuild. The temporary-table checks also assert
the rejected temporary names are not selectable in the same ownerless session.

No product-code change is expected unless the focused test exposes a policy
hole.

## Scope And Non-Goals

In scope:

- Storage-option policy coverage for idempotent, replacement, and temporary
  `CREATE TABLE` spellings.
- Rejection before native InnoDB page-compression, encryption, or table
  `TABLESPACE` paths run.
- Durable file absence checks for rejected table names.
- Same-session non-selectability checks for rejected temporary-table names.
- Compatibility and slice-doc updates.

Out of scope:

- Enabling ownerless page-compressed or encrypted InnoDB tables.
- General tablespace create/drop/alter support.
- `CREATE TABLE ... LIKE` storage-option combinations; MariaDB's grammar routes
  `LIKE` through a separate create body rather than table options.
- Crash injection inside rejected storage-option SQL.
- External MariaDB/RQG storage-option stress.

## Compatibility Impact

No new SQL support is enabled. Ownerless read/write mode becomes more explicit
for additional DDL spellings that could otherwise be mistaken for covered
create-table variants. Ordinary exclusive embedded behavior remains unchanged.

## Directory And Lifecycle Impact

No directory layout changes. The slice strengthens evidence that rejected
ownerless storage-option DDL leaves no durable `.frm` or `.ibd` files under
`datadir/app/` for the rejected names, and that rejected temporary-table
spellings do not leave same-session temporary tables.

## Native Storage Impact

No native storage format changes. The covered options remain unsupported in
ownerless mode until their native file-layout and recovery behavior is
designed.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `table-storage-option-policy`.
- Run adjacent policy selectors:
  - `table-directory-policy`
  - `partition-policy`
  - `tablespace-policy`
  - `compressed-row-format-ddl`
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the ownerless SQL shard containing `table-storage-option-policy`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused selector rejects the additional idempotent, replacement, and
  temporary create-table storage-option spellings with the existing MyLite
  unsupported policy.
- Rejected durable names do not appear in `INFORMATION_SCHEMA.TABLES` and have
  no `.frm`/`.ibd` files under `datadir/app/`.
- Rejected temporary names are not selectable in the same ownerless session.
- Existing baseline rows, quoted storage-option-like column names, and CTAS
  aliases still work through ownerless/native reopen before and after forced
  `.shm` rebuild.
- Compatibility docs describe the broadened storage-option spelling coverage.

## Risks And Unresolved Questions

- This is a deterministic policy-proof slice, not storage-option support.
- Supporting page-compressed, encrypted, or explicit table tablespace ownerless
  tables still requires native file-layout and recovery design.
- External randomized storage-option stress remains planned.
