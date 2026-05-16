# Read-Only Transaction Control

## Problem

MyLite supports bounded direct row-DML transactions, direct `READ WRITE`
transaction starts, and `SET TRANSACTION READ WRITE` controls, but still
rejects MariaDB read-only transaction controls. Applications and frameworks can
use read-only access mode to guard transactional code paths that should not
write. MyLite should accept that control surface for the bounded transaction
scope it already owns and must reject writes while the read-only characteristic
is active.

This slice supports direct read-only transaction access mode for MyLite storage
transactions. It keeps isolation-level changes, `WITH CONSISTENT SNAPSHOT`,
transaction read-only system-variable assignments, XA, release completion, and
transactional DDL beyond the existing explicit rejection out of scope.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documentation for
  [`SET TRANSACTION`](https://mariadb.com/docs/server/reference/sql-statements/transactions/set-transaction)
  and
  [`START TRANSACTION`](https://mariadb.com/docs/server/reference/sql-statements/transactions/start-transaction)
  documents `READ ONLY` and `READ WRITE` access modes. `START TRANSACTION`
  access modes apply to that transaction, while `SET TRANSACTION` can set the
  next-transaction, session, or global default depending on scope.
- `mariadb/sql/sql_yacc.yy:8328-8375` parses
  `START TRANSACTION opt_start_transaction_option_list`, where start options
  include `READ ONLY`, `READ WRITE`, and `WITH CONSISTENT SNAPSHOT`. The
  grammar rejects mixed read-only plus read-write flags.
- `mariadb/sql/sql_yacc.yy:17460-17490` maps `SET TRANSACTION READ ONLY` and
  `READ WRITE` to the `transaction_read_only` system variable with the parsed
  scope.
- `mariadb/sql/sys_vars.cc:4511-4567` defines
  `transaction_read_only` / `tx_read_only`. Unscoped `SET TRANSACTION` cannot
  change the one-shot value inside an active multi-statement transaction, but
  MariaDB can still accept it before a transaction has touched tables, such as
  immediately after `autocommit=0`; session-scoped updates change the session
  default.
- `mariadb/sql/transaction.cc:88-244` applies `READ ONLY` and `READ WRITE`
  flags in `trans_begin()`, marks read-only transaction status, and preserves
  the current access mode for chained transactions.
- `mariadb/sql/sql_parse.cc:5593-5667` resets one-shot isolation and access
  mode after non-chained explicit `COMMIT` / `ROLLBACK`, but starts chained
  transactions with the same characteristics.
- `mariadb/sql/sql_parse.cc:3855-3862` rejects statements flagged with
  `CF_DISALLOW_IN_RO_TRANS` while `thd->tx_read_only` is active.
- `mariadb/sql/set_var.cc:221-224` notes that `transaction_isolation` and
  `transaction_read_only` can be global, session, or one-shot variables rather
  than ordinary session-only variables.

## Design

Extend direct transaction-control state in `libmylite`:

- Add MyLite session flags for:
  - the session default transaction access mode,
  - the next-transaction one-shot access mode,
  - the active transaction access mode.
- Classify direct `START TRANSACTION READ ONLY` as a supported begin control.
  It starts or restarts the bounded MyLite transaction in read-only mode.
- Keep direct `START TRANSACTION READ WRITE` as the explicit read-write begin
  path. Plain `BEGIN` / `START TRANSACTION` use a pending one-shot
  characteristic when present, otherwise the session default.
- Classify direct `SET TRANSACTION READ ONLY` and `SET TRANSACTION READ WRITE`
  as one-shot controls. They update the MyLite one-shot state only after
  MariaDB accepts the statement. If MariaDB accepts the statement while MyLite
  already has an active direct transaction checkpoint, MyLite mirrors the
  characteristic onto the current transaction; this covers the `autocommit=0`
  pre-write case without pretending writes can change access mode later.
- Classify direct `SET SESSION TRANSACTION READ ONLY` /
  `SET LOCAL TRANSACTION READ ONLY` and the existing read-write forms as
  session-default controls. They update the MyLite session default only after
  MariaDB accepts the statement.
- On non-chained `COMMIT` / `ROLLBACK`, reset the active transaction
  characteristic to the session default. On `AND CHAIN` or session
  `completion_type=CHAIN`, preserve the just-finished transaction's access
  mode for the newly opened transaction.
- When `autocommit=0` starts or restarts a MyLite transaction without explicit
  `AND CHAIN`, use the one-shot/default access mode just like a plain
  transaction start.
- Reject direct and prepared MyLite storage writes while an active read-only
  transaction is open. For this slice, storage writes are top-level row DML and
  storage-DDL statements already recognized by MyLite's checkpoint policy.
  Temporary-table write exceptions remain out of scope until the policy is
  table-kind aware.
- Keep `SET transaction_read_only=...`, `SET tx_read_only=...`, global
  transaction defaults, isolation levels, consistent snapshots, mixed read-only
  plus read-write starts, prepared transaction-control statements outside
  existing savepoint support, XA, and release completion rejected.

## Affected Subsystems

- `packages/libmylite`: direct transaction-control classification, transaction
  session state, direct execution write policy, and prepared execution write
  policy.
- Direct SQL, prepared-statement, and storage-engine transaction tests.
- API, storage architecture, compatibility matrix, roadmap, harness, and
  transaction spec docs.

## Compatibility Impact

Applications that use direct read-only transaction controls get a supported
guard against MyLite storage writes in the current bounded transaction scope.
Explicit read-write controls continue to opt into writeable transactions.

Compatibility remains partial:

- isolation levels and consistent snapshots are still unsupported,
- read-only temporary-table write exceptions are not modeled yet,
- handler-level read-only optimization and fully transactional engine flags
  remain planned,
- transaction read-only system-variable assignments remain explicit policy
  failures.

## DDL Metadata Routing Impact

No DDL metadata format changes are introduced. Storage DDL remains rejected
inside active direct MyLite transactions; the later
[Prepared Transactional DDL Policy](../prepared-transactional-ddl-policy/specs.md)
applies the same active-transaction DDL policy to prepared execution before
MariaDB can publish catalog changes.

## Single-File And Embedded Lifecycle

No file-format, journal, lock, or companion-file behavior changes. Read-only
transactions still open the existing bounded MyLite transaction checkpoint so
the lifecycle remains aligned with `COMMIT`, `ROLLBACK`, savepoints, close-time
rollback, and crash-recovery behavior. Because writes are rejected while the
read-only characteristic is active, the checkpoint should remain empty.

## Public API And File Format

The public C API and primary `.mylite` file format do not change. The behavior
is exposed through direct SQL execution and prepared statement execution.

## Storage-Engine Routing Impact

The read-only policy applies to routed durable MyLite storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. It does not add native InnoDB
isolation, consistent snapshot, or optimizer-level read-only behavior.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. Future protocol adapters should
delegate transaction access-mode controls to the core library or mirror the
same one-shot/default/active state before allowing writes.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to parser policy,
session flags, and tests.

## Test And Verification Plan

- Extend direct SQL policy tests to accept read-only direct controls and keep
  isolation, consistent snapshot, global defaults, system-variable assignment,
  mixed access modes, prepared transaction control, and semicolon-chained forms
  rejected.
- Add storage-smoke coverage proving:
  - `START TRANSACTION READ ONLY` allows reads but rejects direct row DML,
  - `SET TRANSACTION READ ONLY` applies to the next plain `BEGIN`,
  - `SET SESSION TRANSACTION READ ONLY` affects later plain transactions,
  - `SET TRANSACTION READ ONLY` after `autocommit=0` affects the current
    pre-write transaction when MariaDB accepts it,
  - direct `READ WRITE` controls override one-shot/session read-only state,
  - non-chained completion resets one-shot access mode,
  - chained completion preserves read-only access mode,
  - prepared row DML execution rejects while a read-only transaction is active.
- Run dev, embedded, storage-smoke, transaction/direct-SQL/prepared-statement
  harness groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct `START TRANSACTION READ ONLY`, `SET TRANSACTION READ ONLY`, and
  `SET SESSION` / `SET LOCAL TRANSACTION READ ONLY` are accepted after MariaDB
  accepts the SQL.
- Read-only active transactions reject direct and prepared MyLite storage
  writes before data changes are published.
- `READ WRITE` transaction starts and defaults continue to produce writeable
  transactions.
- One-shot, session-default, non-chained reset, and chained-preservation
  behavior match MariaDB's documented/source-backed transaction
  characteristic flow for the supported bounded scope.
- Docs and compatibility tables describe read-only transaction support without
  claiming isolation, consistent snapshot, temporary-table exceptions, release
  completion, or fully transactional engine flags.

## Risks And Unresolved Questions

- MyLite still uses conservative SQL scanning rather than MariaDB's parsed
  `LEX` / `set_var` state. Suspicious transaction-control forms remain
  rejected rather than partially interpreted.
- The read-only write policy is top-level statement based. It intentionally
  does not yet distinguish durable MyLite base tables from temporary tables.
- When future work adds isolation or consistent snapshots, the transaction
  characteristic state should be shared rather than duplicated.
