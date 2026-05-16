# Read-Only Temporary Row DML

## Problem

MyLite now enforces read-only transactions for routed MyLite storage writes, but
the policy is SQL-shape based: any `INSERT`, `UPDATE`, `DELETE`, or `REPLACE`
is rejected before MariaDB can resolve whether the target is a durable base
table or a session-local temporary table. MariaDB permits useful temporary-table
workflows inside read-only transactions; MyLite should not block those when the
target is a tracked user temporary table.

This slice allows direct and prepared row DML targeting an existing tracked
temporary table while a read-only MyLite transaction is active. Temporary DDL
inside active transactions was later covered by
[Temporary DDL Transactions](../temporary-ddl-transactions/specs.md), while
durable transactional DDL remains deliberately conservative.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:1480-1520` registers transaction participants and
  records `thd->tx_read_only` in transaction instrumentation.
- `mariadb/sql/handler.cc:1855-2005` coalesces handler read/write state at
  commit and applies read-only restrictions based on participating engines.
- `mariadb/sql/sql_table.cc:4947-5050` creates and opens user temporary
  tables through MariaDB's temporary-table path.
- `mariadb/sql/temporary_tables.cc:605-686` documents explicit temporary-table
  drop and close-time cleanup paths.
- The existing
  [Temporary Table Catalog Isolation](../temporary-table-catalog-isolation/specs.md)
  slice records that MyLite temporary tables are session-local runtime state and
  must not publish durable user-schema catalog entries.
- The existing
  [Read-Only Transaction Control](../read-only-transaction-control/specs.md)
  slice explicitly left temporary-table write exceptions out of scope because
  the policy did not distinguish durable base tables from temporary tables.

## Design

- Track simple unqualified SQL-visible user temporary table names in each
  `mylite_db` handle after successful direct or prepared
  `CREATE TEMPORARY TABLE` statements.
- Remove tracked names after successful direct or prepared
  `DROP [TEMPORARY] TABLE` statements that list those names. Qualified drop
  targets forget the final identifier conservatively so stale temporary state
  cannot allow a later durable same-name write.
- Recognize single-target direct and prepared row DML:
  - `INSERT [modifiers] [INTO] table_name`
  - `REPLACE [modifiers] [INTO] table_name`
  - `UPDATE [modifiers] table_name ...`
  - `DELETE [modifiers] FROM table_name ...`
- If an active transaction is read-only, allow those row-DML statements only
  when their target table name is currently tracked as temporary.
- Keep schema-qualified targets, multi-table DML, unknown target shapes, and
  durable table targets rejected before MariaDB execution while the transaction
  is read-only.
- Store prepared SQL text so the read-only execution gate can evaluate the
  current temporary-table state at `mylite_step()` time instead of freezing
  the decision at prepare time.
- Keep durable DDL inside active direct transactions rejected by the existing
  transactional DDL policy. The
  [Temporary DDL Transactions](../temporary-ddl-transactions/specs.md) slice
  covers explicit temporary table create/drop inside active transactions.

## Affected Subsystems

- `packages/libmylite`: embedded SQL policy, temporary table tracking, prepared
  statement execution state.
- Storage-engine transaction tests.
- API, storage architecture, compatibility matrix, roadmap, and related specs.

## Compatibility Impact

Applications can use session-local temporary tables for row writes inside
read-only transactions without disabling MyLite's durable table protection.
This is partial compatibility: only simple single-target temporary row DML is
accepted; explicit temporary table create/drop inside active transactions is
covered separately.

## DDL Metadata Routing Impact

No durable catalog metadata changes. Tracking is session-local in-memory state
that mirrors temporary table lifecycle enough for the read-only policy gate.

## Single-File And Embedded Lifecycle

No file-format changes. Temporary table writes may still use MyLite temporary
storage identities under the existing temporary-table lifecycle; this slice
does not make temporary storage durable.

## Public API And File Format

No C API or `.mylite` file-format changes. The behavior is exposed through
`mylite_exec()` and `mylite_step()` allowing additional row DML in active
read-only transactions.

## Storage-Engine Routing Impact

Durable MyLite-routed tables remain protected by read-only transaction
rejection. Tracked temporary tables are allowed only for the simple row-DML
target shapes above.

## Wire Protocol Or Integration Impact

No wire-protocol package changes.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to a small SQL target parser,
temporary-name tracking, and tests.

## Test And Verification Plan

- Add storage-smoke coverage for direct `INSERT`, `UPDATE`, `DELETE`, and
  `REPLACE` against a temporary table inside a read-only transaction.
- Add prepared statement coverage for temporary row DML inside a read-only
  transaction.
- Keep durable table direct and prepared row DML rejected inside read-only
  transactions.
- Verify temporary names are removed after direct and prepared
  `DROP TEMPORARY TABLE` so later same-name durable writes do not bypass the
  read-only gate.
- Run dev, embedded, storage-smoke, transaction harness, formatting, tidy,
  shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct and prepared simple single-target row DML against tracked temporary
  tables succeeds inside active read-only transactions.
- Durable table writes and unknown/multi-table row-DML target shapes remain
  rejected inside active read-only transactions.
- Dropped temporary table names no longer bypass the read-only gate.
- Docs describe this as a temporary row-DML exception, not broad temporary DDL
  or real storage isolation support.

## Risks And Unresolved Questions

- SQL parsing remains intentionally bounded to unqualified temporary table
  names. Broader DML grammar should move toward MariaDB-resolved table metadata
  rather than expanding ad hoc parsing.
- The temporary-name tracker is session-local and intentionally not persisted.
  It must not be used as durable catalog metadata.
