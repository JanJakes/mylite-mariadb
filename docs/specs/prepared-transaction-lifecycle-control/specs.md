# Prepared Transaction Lifecycle Control

## Goal

Allow prepared `BEGIN`, `START TRANSACTION`, `COMMIT`, and `ROLLBACK` controls
for the bounded MyLite row-DML transaction scope that direct execution already
supports.

## Non-Goals

- `WITH CONSISTENT SNAPSHOT`.
- `RELEASE` completion or `completion_type=RELEASE/2`.
- XA.
- Durable transactional DDL.
- Handler-level MariaDB transaction or savepoint hooks.
- Storage isolation guarantees beyond the existing read-only write rejection.
- Parameterized transaction-control values.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8328-8359` parses
  `START TRANSACTION opt_start_transaction_option_list` as `SQLCOM_BEGIN` and
  rejects mixed `READ ONLY` plus `READ WRITE`.
- `mariadb/sql/sql_yacc.yy:18298-18318` parses
  `COMMIT [WORK] opt_chain opt_release` and
  `ROLLBACK [WORK] opt_chain opt_release`; `AND CHAIN RELEASE` is rejected.
- `mariadb/sql/transaction.cc:342-356` resets one-shot transaction
  characteristics after non-chained explicit transaction completion.
- `mariadb/mysql-test/main/ps_missed_cmds.test` test case 7 verifies that
  `BEGIN`, `SAVEPOINT`, and `RELEASE SAVEPOINT` are supported as MariaDB
  prepared statements. MyLite already has a MyLite-owned prepared savepoint
  path; this slice extends the remaining bounded lifecycle commands.
- The existing MyLite classifier already accepts direct `BEGIN`,
  `START TRANSACTION`, `START TRANSACTION READ WRITE`, `START TRANSACTION READ
  ONLY`, `COMMIT`, `ROLLBACK`, `AND CHAIN`, `AND NO CHAIN`, and `NO RELEASE`
  forms, and rejects unsupported direct lifecycle syntax before MariaDB
  execution.

## Compatibility Impact

Applications and frameworks that prepare transaction lifecycle SQL can now use
the same bounded row-DML transaction controls as direct execution:

- `BEGIN` / `BEGIN WORK`,
- `START TRANSACTION`,
- `START TRANSACTION READ WRITE`,
- `START TRANSACTION READ ONLY`,
- `COMMIT` / `ROLLBACK`,
- supported `AND CHAIN`, `AND NO CHAIN`, and `NO RELEASE` completion forms.

The support remains bounded to MyLite's current row-DML transaction model.
Global transaction defaults, consistent snapshots, release completion, XA,
parameterized transaction-control values, durable transactional DDL, real
storage isolation, and handler-level transaction flags remain unsupported.

## Design

During `mylite_prepare()`:

- keep the existing transaction-control classifier,
- keep MyLite-owned prepared savepoint handling unchanged,
- allow the same begin and completion transaction-control kinds that direct
  execution supports to pass to MariaDB's prepared statement API,
- continue to reject unsupported lifecycle syntax before MariaDB prepare.

During `mylite_step()`:

- execute the prepared statement through `mysql_stmt_execute()`,
- after MariaDB succeeds, call the same MyLite transaction-state mirror used
  by direct execution and prepared transaction `SET` controls,
- preserve warnings, affected rows, insert ids, reset/finalize ownership, and
  statement checkpoint behavior from the ordinary prepared non-result path.

## File Lifecycle

No file-format change is introduced. Prepared begin controls open the same
outer MyLite transaction checkpoint and transient
`<database>.mylite-transaction-journal` used by direct row-DML transactions.
Prepared commit removes that journal as the durable commit point; prepared
rollback restores the transaction-start state.

## Embedded Lifecycle And API

No public C API change. The behavior is visible through existing
`mylite_prepare()`, `mylite_step()`, `mylite_reset()`, and
`mylite_finalize()`.

## Build, Size, And Dependencies

No dependency or build-profile change.

## Test Plan

- Extend prepared-statement policy coverage so supported prepared lifecycle
  statements prepare and execute, while unsupported transaction controls still
  fail with MyLite policy diagnostics.
- Add storage-smoke coverage proving prepared `BEGIN`/`ROLLBACK`, prepared
  `BEGIN`/`COMMIT`, prepared `START TRANSACTION READ ONLY`, and prepared
  chained completion over routed durable rows.
- Run targeted embedded and storage-engine tests plus formatting, shell syntax,
  and whitespace checks.

## Acceptance Criteria

- Prepared lifecycle controls that have direct support succeed through
  `mylite_prepare()` and `mylite_step()`.
- Prepared `ROLLBACK` restores routed MyLite row-DML transaction changes.
- Prepared `COMMIT` publishes routed MyLite row-DML transaction changes.
- Prepared chained completion commits or rolls back the current transaction and
  leaves the next transaction active with the expected access mode.
- Unsupported lifecycle controls remain rejected before MariaDB execution.
- Docs and compatibility tables no longer describe prepared transaction start
  and completion as unsupported.

## Risks And Open Questions

- This still relies on the current MyLite-owned outer checkpoint model. It does
  not make the MariaDB handler advertise transactional engine flags.
- MariaDB accepts more prepared statements than MyLite should support in the
  embedded single-file profile; this slice only expands transaction lifecycle
  controls, not server or metadata surfaces.
