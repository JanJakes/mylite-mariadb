# MTR Storage Schema Lifecycle Smoke

## Problem

The storage-routed MTR suite covers routed table DDL/DML, temporary tables, and
sidecar absence, but it does not yet exercise catalog-backed schema namespace
behavior in raw embedded MTR. MyLite already has C-level storage-smoke coverage
for directory-free schemas, `CREATE TABLE IF NOT EXISTS`, and `DROP TABLE IF
EXISTS`; the MTR storage profile should also prove representative schema
namespace and table-existence paths against a primary `.mylite` file.

## Source Findings

Base: MariaDB `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:2691`, `:5655`, `:8713`, and `:13424` parse
  `CREATE DATABASE`, `CREATE TABLE IF NOT EXISTS`, `RENAME TABLE IF EXISTS`,
  and `DROP TABLE IF EXISTS` into the normal MariaDB DDL option bits.
- `mariadb/sql/sql_db.cc:790` contains the MyLite schema hook branch for
  catalog-backed schema create paths when runtime schema directories are
  absent.
- `mariadb/sql/sql_table.cc:4723` and `:4820` own existing-table skip behavior
  for `CREATE TABLE IF NOT EXISTS`; `mariadb/sql/sql_table.cc:1830-1868` owns
  missing-table skip behavior for `DROP TABLE IF EXISTS`.
- `mariadb/sql/sql_rename.cc:277` and `:506-565` treat missing
  `RENAME TABLE IF EXISTS` source tables as warning-producing skipped pairs
  and continue with existing pairs.
- Existing MyLite specs already define the product behavior for
  `CREATE DATABASE` existence options, directory-free schema creation,
  `CREATE TABLE IF NOT EXISTS`, and `DROP TABLE IF EXISTS`; this slice adds raw
  MTR coverage rather than new production behavior.

## Scope

- Add one MyLite-owned storage-routed MTR test for two catalog-backed schemas.
- Cover same-name routed tables in different schemas.
- Cover duplicate `CREATE TABLE IF NOT EXISTS` preserving table definition,
  requested engine metadata, rows, and supported index reads.
- Cover missing-target `CREATE TABLE IF NOT EXISTS` publication.
- Cover mixed `RENAME TABLE IF EXISTS` and `DROP TABLE IF EXISTS` statements.
- Keep durable sidecar checks for both schemas.

## Non-Goals

- Do not implement new schema DDL behavior.
- Do not cover views, triggers, routines, events, partitions, temporary-table
  existence options, or transactional schema DDL.
- Do not normalize warning text in MTR; existing public warning API coverage
  remains the authoritative warning-shape test.

## Compatibility Impact

The slice adds compatibility evidence for existing partial MariaDB schema and
table DDL behavior under the storage-routed MTR profile. It does not broaden
the supported SQL surface.

## Single-File And Lifecycle Impact

The test must prove that the covered schemas and routed tables do not publish
runtime schema directories or MariaDB durable sidecars. The only durable state
should remain the primary `.mylite` file plus allowed MyLite recovery
companions.

## Test And Verification Plan

- Add `mylite.routed_storage_schema_lifecycle` to the storage MTR list.
- Run the new test through `tools/mylite-mtr-harness run-storage`.
- Run the full storage-routed MTR list.
- Run MTR coverage inventory and shell/diff checks.

## Acceptance Criteria

- Same-name routed tables in different schemas keep independent rows and
  requested-engine metadata.
- Duplicate `CREATE TABLE IF NOT EXISTS` does not replace an existing routed
  table.
- Missing `CREATE TABLE IF NOT EXISTS` creates a routed table.
- Mixed `RENAME TABLE IF EXISTS` skips the missing pair and renames the
  existing table.
- `DROP TABLE IF EXISTS` removes existing targets and tolerates missing ones.
- Sidecar checks pass before and after cleanup.
