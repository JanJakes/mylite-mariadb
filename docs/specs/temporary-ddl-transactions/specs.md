# Temporary DDL Transactions

## Problem

MyLite currently rejects every storage-DDL-shaped statement while a bounded
direct transaction is active. That is correct for durable MyLite tables because
the current transaction layer only proves row-DML rollback, but it is too broad
for user temporary tables. MariaDB treats user temporary table create/drop as
session-local work that does not commit the active transaction, and MyLite
already tracks temporary table names for read-only row-DML exceptions.

This slice allows explicit temporary table create/drop inside active direct
transactions without allowing durable transactional DDL.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_parse.cc:529-536` marks `SQLCOM_CREATE_TABLE` as
  auto-commit DDL at the command level, but later command execution uses
  `LEX::tmp_table()` to distinguish temporary-table paths.
- `mariadb/sql/sql_parse.cc:3810-3833` commits active normal transactions
  before commands that cause implicit commit. MyLite intentionally does not
  mirror that for durable DDL yet.
- `mariadb/sql/sql_table.cc:4700-4740` handles an existing user temporary table
  before checking durable base tables and drops only the temporary table for
  `CREATE OR REPLACE TEMPORARY TABLE`.
- `mariadb/sql/sql_table.cc:4947-5050` creates and opens user temporary tables
  through `THD::create_and_open_tmp_table()` rather than publishing durable
  table metadata.
- `mariadb/sql/sql_table.cc:1327-1344` notes that temporary table drops do not
  commit an ongoing transaction and are treated separately from normal table
  drops.
- `mariadb/sql/temporary_tables.cc:605-686` documents explicit temporary-table
  drop and close-time cleanup.
- `mariadb/sql/sql_prepare.cc:2290-2315` accepts `SQLCOM_CREATE_TABLE` and
  `SQLCOM_DROP_TABLE` as preparable statements, so MyLite's prepared execution
  policy must use execution-time transaction state.

## Design

- Keep durable storage DDL rejected while an active direct MyLite transaction
  exists.
- Recognize explicit temporary table DDL:
  - `CREATE [OR REPLACE] TEMPORARY TABLE ...`,
  - `DROP TEMPORARY TABLE ...`.
- Let those statements execute through direct and prepared APIs inside active
  transactions.
- Do not open MyLite durable statement checkpoints for explicit temporary DDL.
  Temporary table rows and indexes must not be part of the outer durable
  transaction journal.
- Move MyLite handler storage for user temporary tables onto the existing
  volatile row/index store. Durable routed base tables and durable
  `ENGINE=MEMORY` / `ENGINE=HEAP` metadata keep their existing behavior.
- Keep SQL-visible temporary table name tracking session-local and update it
  only after successful create/drop execution.
- Keep `ALTER TABLE`, `RENAME TABLE`, `TRUNCATE TABLE`, non-temporary `DROP`,
  and durable `CREATE` rejected inside active transactions until full
  transactional DDL or explicit implicit-commit semantics are designed.

## Affected Subsystems

- `packages/libmylite`: direct/prepared SQL policy, checkpoint selection, and
  temporary table tracking.
- `mariadb/storage/mylite`: user temporary table handler storage selection.
- Storage-engine smoke tests and transaction compatibility docs.

## Compatibility Impact

Applications can create, populate, and drop session-local temporary tables
inside active MySQL/MariaDB-style transactions without forcing a commit or
weakening durable table protection. This covers common setup and staging-table
workflows while preserving MyLite's explicit unsupported status for durable
transactional DDL.

The slice intentionally requires the `TEMPORARY` keyword for DDL policy
relaxation. `DROP TABLE temp_name` can resolve to a temporary table in MariaDB,
but MyLite keeps that form rejected inside active transactions until the policy
uses resolved table metadata instead of SQL-shape checks.

## DDL Metadata Routing Impact

User temporary tables do not publish durable user-schema catalog records. The
handler still uses MariaDB's in-memory temporary table definition, while rows,
indexes, and autoincrement state live in MyLite's process-local volatile table
store for the temporary storage identity.

## Single-File And Embedded Lifecycle

No `.mylite` file-format change. User temporary table data is no longer written
to durable primary-file table, row, index, or autoincrement pages. Explicit
`DROP TEMPORARY TABLE`, handle close, and embedded runtime cleanup remove the
session-local state.

## Public API And File Format

No C API or file-format change. Behavior is visible through `mylite_exec()`,
`mylite_prepare()`, and `mylite_step()`.

## Storage-Engine Routing Impact

Explicit temporary tables still route through the MyLite handler when the
effective engine is MyLite. Durable requested-engine metadata is unchanged.
The temporary row store is a MyLite implementation detail, not durable
`ENGINE=MEMORY` semantics.

## Wire Protocol Or Integration Impact

No wire-protocol package changes. Future protocol adapters should inherit this
core direct/prepared policy.

## Binary-Size And Dependency Impact

No dependency or default build-profile change.

## Test And Verification Plan

- Add storage-smoke coverage for direct `CREATE TEMPORARY TABLE` inside an
  active direct transaction, followed by direct/prepared temporary row DML and
  outer `ROLLBACK`.
- Add prepared `CREATE TEMPORARY TABLE` and `DROP TEMPORARY TABLE` execution
  inside active transactions.
- Verify temporary tables created inside a rolled-back durable transaction
  remain session-local and usable, while durable row DML in the outer
  transaction rolls back.
- Verify explicit temporary drops inside active transactions remain dropped
  after outer rollback.
- Keep durable direct/prepared DDL rejected inside active transactions.
- Run focused storage-smoke, transaction compatibility harness, dev build,
  formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct and prepared explicit temporary table DDL succeeds inside active direct
  transactions.
- Durable direct and prepared DDL remains rejected inside active direct
  transactions.
- Temporary rows created inside the active transaction remain available after
  outer `ROLLBACK`; durable rows created in the same outer transaction roll
  back.
- Dropped temporary tables remain dropped after outer `ROLLBACK`.
- User temporary table data does not require durable primary-file table
  definitions or row/index pages.

## Risks And Unresolved Questions

- The SQL policy is still deliberately bounded. Broader compatibility for
  `DROP TABLE` resolving to temporary tables, `ALTER TABLE` over temporary
  tables, and resolved-metadata DDL policy belongs in a later slice.
- The MyLite handler still advertises non-transactional flags. This slice does
  not claim general transactional DDL, real isolation, or handler-level
  transactional engine semantics.
