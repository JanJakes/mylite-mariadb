# Prepared Schema DDL Sync

## Problem

Schema namespace records are synchronized after successful direct
`CREATE/DROP DATABASE` and `CREATE/DROP SCHEMA`, but the public prepared
statement API executes non-result SQL through a separate path. A successful
prepared schema DDL statement can therefore create or drop a transient MariaDB
runtime directory without updating the durable `.mylite` catalog.

This slice keeps prepared schema DDL consistent with direct execution.

## Source Findings

Base authority: MariaDB 11.8.6, initial import ref
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- `packages/libmylite/src/database.cc` routes direct SQL through
  `exec_impl()`, captures warnings, then calls `sync_schema_catalog()` for
  successful schema DDL.
- The same file routes prepared statements through `prepare_impl()` and
  `execute_statement()`. Successful non-result execution captures affected rows,
  insert id, and warnings before returning `MYLITE_DONE`.
- MariaDB's prepared-statement API can prepare and execute static DDL text.
  Identifiers are not parameterized, so this slice only needs to detect schema
  DDL from the prepared SQL text supplied to `mylite_prepare()`.

## Design

- Reuse the direct SQL schema-DDL detector for prepared statements.
- Store a statement-level boolean at prepare time indicating that a successful
  execution should synchronize the schema catalog.
- After successful non-result prepared execution and warning capture, call
  `sync_schema_catalog()` for file-backed MyLite storage-engine builds.
- Do not sync result-producing statements, failed prepare, failed execute, or
  `:memory:` handles.

## Compatibility Impact

Covered in this slice:

- Prepared `CREATE SCHEMA` / `CREATE DATABASE` persists the namespace in the
  `.mylite` catalog after successful execution.
- File-backed reopen rehydrates prepared-created schemas before user SQL.
- Prepared `DROP SCHEMA` / `DROP DATABASE` removes the durable namespace record.

Still planned:

- Persistent schema options such as default character set, collation, and
  comments.
- Final SQL-layer database hooks that remove the temporary directory bridge.
- SQL transaction rollback semantics for schema DDL.

## Single-File And Embedded Lifecycle

Durable namespace state remains in the primary `.mylite` file. Prepared schema
DDL still uses MariaDB's transient runtime directory operations, then mirrors
the resulting runtime schema list into the catalog.

## Public API And File Format

No public API or file-format change is required. The implementation adds only
prepared-execution routing to the schema catalog sync introduced by the schema
namespace slice.

## Test Plan

- Extend the storage-engine smoke test with prepared `CREATE SCHEMA` and
  prepared `DROP SCHEMA`.
- Verify catalog presence after prepared create, reopen `USE`, catalog removal
  after prepared drop, and sidecar cleanup.
- Run storage-smoke, embedded, dev, formatting, tidy, compatibility report, and
  size checks.

## Acceptance Criteria

- Prepared schema DDL has the same durable namespace behavior as direct schema
  DDL for file-backed MyLite storage-engine builds.
- Existing prepared statement behavior, warnings, affected rows, and insert id
  coverage continue to pass.
- Compatibility and roadmap docs no longer list prepared schema DDL sync as a
  remaining schema namespace gap.
