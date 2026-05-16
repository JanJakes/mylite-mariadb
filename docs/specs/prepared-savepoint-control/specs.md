# Prepared Savepoint Control

Status note: the later
[Transaction Modifier Control](../transaction-modifier-control/specs.md) slice
adds bounded direct transaction-start and completion modifier support. Prepared
`BEGIN`, `COMMIT`, `ROLLBACK`, and transaction-start or completion modifiers
remain unsupported. The later
[Prepared Transaction SET Control](../prepared-transaction-set-control/specs.md)
slice adds prepared `SET` transaction controls. Later quoted-name slices add
SQL-mode-aware double-quoted identifiers and case-insensitive savepoint lookup.

## Problem

MyLite supports direct `SAVEPOINT`, `ROLLBACK TO [SAVEPOINT]`, and
`RELEASE SAVEPOINT` inside bounded direct row-DML transactions, but prepared
savepoint-control SQL is still rejected. Applications and adapters can prepare
transaction-control statements once and execute them repeatedly, so this leaves
a small but real gap in the current `ENGINE=InnoDB`-routes-to-MyLite
transaction surface.

This slice adds prepared savepoint-control statements for the same bounded
file-backed row-DML transaction scope. It does not add prepared `BEGIN`,
`COMMIT`, `ROLLBACK`, transaction modifiers, XA, handler-level savepoint hooks,
transactional DDL, isolation, or transactional engine flags. At this slice
point, prepared transaction `SET` controls were still unsupported; the later
prepared transaction SET slice narrows that gap.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `SAVEPOINT ident`, `ROLLBACK ... TO
  [SAVEPOINT] ident`, and `RELEASE SAVEPOINT ident` into
  `SQLCOM_SAVEPOINT`, `SQLCOM_ROLLBACK_TO_SAVEPOINT`, and
  `SQLCOM_RELEASE_SAVEPOINT`.
- `mariadb/sql/sql_parse.cc` executes those commands through
  `trans_savepoint()`, `trans_rollback_to_savepoint()`, and
  `trans_release_savepoint()`.
- `mariadb/sql/transaction.cc:trans_savepoint()` creates or replaces a SQL
  savepoint and calls `ha_savepoint()`.
- `mariadb/sql/handler.cc:ha_savepoint()` requires every registered
  transaction participant to provide `savepoint_set`; missing hooks report
  unsupported `SAVEPOINT`.
- `mariadb/sql/handler.cc:ha_rollback_to_savepoint()` and
  `ha_release_savepoint()` delegate rollback/release to participant
  savepoint hooks.
- `mariadb/sql/handler.h` exposes `savepoint_offset`, `savepoint_set`,
  `savepoint_rollback`, and `savepoint_release` as handler-level savepoint
  integration points.

Because the MyLite handler still advertises non-transactional engine flags and
does not provide handler-level savepoint hooks, prepared savepoint support must
use the same `libmylite` policy path as direct savepoint control for now.

## Design

Recognize prepared savepoint-control SQL during `mylite_prepare()` when the
MariaDB-backed MyLite storage engine is available:

- `SAVEPOINT name`, `ROLLBACK TO [SAVEPOINT] name`, and
  `RELEASE SAVEPOINT name` create a MyLite-owned prepared statement object with
  no `MYSQL_STMT`.
- The prepared statement has zero parameters and zero result columns.
- `mylite_step()` executes the stored control operation through the existing
  `execute_direct_savepoint_control()` checkpoint stack.
- Execution requires an active file-backed direct row-DML transaction.
  Preparing can happen before the transaction starts, matching ordinary
  prepared-statement lifetime expectations.
- `mylite_reset()` makes the control statement executable again.
- `mylite_finalize()` releases the statement without asking MariaDB to close a
  non-existent prepared handle.

At this slice point, prepared `BEGIN`, `COMMIT`, `ROLLBACK`,
`SET autocommit`, `SET TRANSACTION`, XA, and transaction modifiers remained
policy failures. The later prepared transaction SET slice keeps lifecycle
controls rejected but supports bounded prepared transaction `SET` controls.

## Affected Subsystems

- `packages/libmylite`: prepared statement representation and execution path.
- `packages/libmylite` embedded and storage-engine tests.
- API, storage architecture, compatibility, harness, and roadmap docs.

## Compatibility Impact

Applications can prepare and reuse simple savepoint-control statements inside
the current bounded MyLite transaction scope. The row-DML behavior matches the
direct savepoint path: `ROLLBACK TO` restores the target snapshot and keeps the
target savepoint active, `RELEASE` preserves changes while removing the target,
and full transaction rollback still unwinds all savepoints.

Compatibility remains partial:

- Savepoint names support the current simple unquoted parser, with
  backtick-quoted identifiers added by the later
  [Quoted Savepoint Names](../quoted-savepoint-names/specs.md) slice.
- Execution outside an active file-backed MyLite transaction fails explicitly.
- Handler-level savepoint hooks and fully transactional engine flags remain
  planned.
- MEMORY/HEAP volatile-row savepoints, transactional DDL, isolation, XA, and
  unsupported transaction modifiers remain unsupported.

## DDL Metadata Routing Impact

No catalog format change is introduced. DDL remains rejected while a direct
transaction checkpoint is active, including when prepared savepoint statements
exist or savepoints are active.

## Single-File And Embedded Lifecycle

No new durable companion file is introduced. Prepared savepoint statements
reuse the existing direct transaction checkpoint stack and transaction journal.
The prepared statement object owns only the parsed savepoint name and control
kind.

## Public API And File Format

The public C API and primary `.mylite` file format do not change. Prepared
savepoint statements are exposed through existing `mylite_prepare()`,
`mylite_step()`, `mylite_reset()`, and `mylite_finalize()` calls.

## Storage-Engine Routing Impact

The behavior applies to durable MyLite-routed row storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. It does not expand the
transaction claim for BLACKHOLE, MEMORY/HEAP, or future native engine-specific
semantics.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future wire-protocol wrapper
can map prepared savepoint-control commands onto the public `libmylite`
prepared-statement behavior until handler-level savepoint hooks exist.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to one small prepared
statement branch and stored savepoint names.

## Test And Verification Plan

- Extend prepared-statement policy tests to accept prepared savepoint control
  only where the MyLite storage engine is present and to keep explicit
  transaction-control diagnostics otherwise.
- Add storage-smoke coverage proving:
  - prepared savepoint statements can be prepared before a transaction,
  - execution outside a transaction fails explicitly,
  - prepared `SAVEPOINT`, `ROLLBACK TO`, and `RELEASE` work inside a routed
    `ENGINE=InnoDB` transaction,
  - `mylite_reset()` allows reuse of a prepared savepoint-control statement,
  - prepared `ROLLBACK TO` keeps the target savepoint active.
- Run dev, embedded, storage-smoke, transaction and prepared-statement harness
  groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Prepared savepoint-control statements work for bounded file-backed MyLite
  row-DML transactions.
- Prepared transaction-control statements outside this savepoint scope remain
  rejected.
- Prepared savepoint execution uses the same checkpoint semantics as direct
  savepoint execution.
- Tests and docs reflect the partial compatibility scope without claiming full
  handler-level transaction support.

## Risks And Unresolved Questions

- Prepared savepoint support is intentionally implemented in `libmylite` rather
  than MariaDB handler hooks. This is correct for the current non-transactional
  handler flags, but it remains a compatibility bridge until handler-level
  savepoint hooks can be implemented honestly.
- SQL-mode-sensitive double-quoted identifiers and case-insensitive lookup are
  covered by later quoted-name slices rather than this prepared-statement slice.
