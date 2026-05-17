# Autocommit Duplicate Control

## Goal

Allow direct and prepared session `SET` lists to assign supported
`autocommit` values more than once, matching MariaDB's ordered `SET` update
behavior where assignments run in list order and the final assignment
determines the effective session state.

## Non-Goals

- Global autocommit assignments.
- Parameterized transaction-control values such as `SET autocommit=?` at this
  slice point.
- Mixing autocommit assignments with transaction read-only assignments in the
  same `SET` list.
- Prepared `BEGIN`, `START TRANSACTION`, `COMMIT`, or `ROLLBACK`.
- Durable transactional DDL, storage isolation guarantees, or release
  completion semantics.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/set_var.cc:733-750` checks the full variable list, rewinds the
  iterator, and updates assignments in list order.
- `mariadb/sql/sys_vars.cc:4805-4869` implements `autocommit` updates:
  enabling autocommit commits open transactions, disabling autocommit marks
  the session as not autocommit, and unchanged values are no-ops.
- `mariadb/sql/sys_vars.cc:4828-4836` specifically preserves statement-scope
  cleanup until the end of multi-assignment `SET` statements that assign
  `@@autocommit`.
- `mariadb/sql/sql_prepare.cc:2394-2395` allows `SQLCOM_SET_OPTION` in the
  prepared-statement validation path.

## Compatibility Impact

This removes a MyLite-only restriction for ordinary session setup SQL.
Supported session autocommit assignments may repeat in direct and prepared
`SET` lists. The final supported assignment drives MyLite's mirrored
autocommit-disabled transaction checkpoint state after MariaDB accepts the
statement, while intermediate `autocommit=1` assignments still commit any
active autocommit-disabled MyLite transaction before later assignments run.

Global autocommit assignment remains unsupported because it mutates
process-global server state rather than the file-owned `libmylite` session
contract. The later
[Prepared Parameterized Transaction SET Control](../prepared-parameterized-transaction-set-control/specs.md)
slice supports single-marker prepared transaction `SET` values by resolving the
bound transition before MariaDB prepared execution.

## Design

Update `direct_set_assignment_transaction_control_kind()` so a second
supported session autocommit assignment replaces the previously classified
autocommit result instead of rejecting the whole `SET` list.

After MariaDB accepts the full statement, walk the supported autocommit
assignments in list order and apply each MyLite transaction transition:

- `autocommit=1` / `ON` / `TRUE` / `DEFAULT` commits an active
  autocommit-disabled MyLite transaction,
- `autocommit=0` / `OFF` / `FALSE` opens or keeps the row-DML transaction
  checkpoint active,
- the final supported assignment leaves the mirrored session autocommit state.

Keep the existing conservative boundaries:

- unsupported autocommit scopes or values still reject the statement,
- a list that mixes autocommit with transaction read-only assignment still
  rejects,
- semicolon tails remain unsupported transaction control,
- completion-type and transaction-isolation assignments keep their existing
  behavior.

Prepared statements use the same classifier and already record the resulting
transaction-control kind for post-execute mirroring, so accepting duplicate
autocommit lists in the classifier covers both direct and prepared execution.

## File Lifecycle

No file-format or new companion-file behavior changes. Final `autocommit=0`
opens the existing outer row-DML transaction checkpoint and transaction journal;
final `autocommit=1` or `DEFAULT` commits the existing checkpoint through the
current direct/prepared transaction-control path.

## Embedded Lifecycle And API

No public C API change. Behavior is visible through existing `mylite_exec()`,
`mylite_prepare()`, and `mylite_step()` calls.

## Build, Size, And Dependencies

No dependency or build-profile change. Binary-size impact is limited to one
branch-policy relaxation and tests.

## Test Plan

- Direct SQL policy tests accept repeated supported session autocommit
  assignments and keep the final value as supported state.
- Prepared SQL policy tests accept the same repeated supported forms and still
  reject global or parameterized autocommit controls.
- Storage-smoke tests prove:
- final `autocommit=1` commits an active row-DML transaction,
- final `autocommit=0` keeps a rollbackable row-DML transaction active,
- an intermediate `autocommit=1` commits the prior active transaction before a
  later `autocommit=0` opens a new rollbackable transaction,
  - final `autocommit=DEFAULT` commits through the default-on path,
  - prepared duplicate-autocommit lists mirror the same final-value behavior.
- Run storage-smoke direct/prepared statement tests, dev storage tests, format
  checks, and whitespace checks.

## Acceptance Criteria

- `SET autocommit=0, autocommit=1` succeeds and leaves autocommit on.
- `SET autocommit=1, autocommit=0` succeeds and leaves a rollbackable MyLite
  row-DML transaction active.
- When an autocommit-disabled transaction is active,
  `SET autocommit=1, autocommit=0` commits the previous transaction and leaves
  a new rollbackable MyLite row-DML transaction active.
- Prepared duplicate supported session autocommit lists succeed and mirror
  the same post-execute transaction state.
- Global, parameterized, mixed autocommit/read-only, and chained-statement
  controls remain explicit transaction-control failures.

## Risks And Open Questions

- MyLite still scans SQL text conservatively instead of consuming MariaDB's
  parsed `set_var` list. Suspicious transaction-control targets continue to
  reject rather than partially applying inferred state.
- Full storage isolation and durable transactional DDL remain separate
  transaction/recovery roadmap work.
