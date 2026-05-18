# Transaction Modifier Control

Status note: the later
[Transaction SET No-Op Control](../transaction-set-noop-control/specs.md)
slice accepts direct `SET TRANSACTION READ WRITE` and session
`SET completion_type=NO_CHAIN/0/DEFAULT` as no-op controls for the same
bounded read-write, no-chain transaction scope. The later
[Completion Type Chain Control](../completion-type-chain-control/specs.md)
slice accepts session `SET completion_type=CHAIN/1` and mirrors chained plain
completion. The later
[Read-Only Transaction Control](../read-only-transaction-control/specs.md)
slice accepts bounded direct read-only access mode. The later
[Completion-Type Duplicate Control](../completion-type-duplicate-control/specs.md)
slice accepts duplicate supported `completion_type` assignments with the final
assignment winning. Release completion defaults and global defaults remain
unsupported; the later
[Transaction Isolation Control](../transaction-isolation-control/specs.md)
slice accepts direct/session isolation controls as compatibility setup SQL
without claiming storage isolation semantics. The later
[Prepared Transaction SET Control](../prepared-transaction-set-control/specs.md)
slice supports MariaDB-preparable transaction `SET` controls while prepared
transaction-start and completion commands remain unsupported. The later
[Prepared Transaction Lifecycle Control](../prepared-transaction-lifecycle-control/specs.md)
slice accepts the bounded prepared lifecycle subset. The later
[Transaction Lifecycle Rejection Matrix](../transaction-lifecycle-rejection-matrix/specs.md)
slice adds routed-storage coverage for the remaining unsupported consistent
snapshot, release completion, release completion-default, and XA forms.

## Problem

MyLite supports a bounded direct row-DML transaction surface through plain
`BEGIN`, `START TRANSACTION`, `COMMIT`, `ROLLBACK`, supported session
`SET autocommit=0/1/DEFAULT`, and direct or prepared savepoint control. It still
rejects MariaDB transaction modifiers such as `START TRANSACTION READ WRITE` and
`COMMIT AND CHAIN`.

Applications that emit explicit read-write transaction starts or completion
modifiers should work when the requested semantics fit MyLite's current
checkpoint-backed row-DML transaction model. Modifier forms that require
read-only enforcement, consistent snapshots, connection release, XA, isolation
level handling, or server-global completion defaults must remain explicit
unsupported surfaces.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses
  `START TRANSACTION opt_start_transaction_option_list`; options are
  `WITH CONSISTENT SNAPSHOT`, `READ ONLY`, and `READ WRITE`, and the grammar
  rejects mixed `READ ONLY` plus `READ WRITE`.
- `mariadb/sql/transaction.cc:trans_begin()` commits any active multi-statement
  transaction first, applies `MYSQL_START_TRANS_OPT_READ_ONLY` or
  `MYSQL_START_TRANS_OPT_READ_WRITE`, sets `OPTION_BEGIN`, and marks the server
  status as in-transaction.
- `mariadb/sql/sql_yacc.yy` parses
  `COMMIT [WORK] opt_chain opt_release` and
  `ROLLBACK [WORK] opt_chain opt_release`. `opt_chain` accepts
  `AND CHAIN` and `AND NO CHAIN`; `opt_release` accepts `RELEASE` and
  `NO RELEASE`; `AND CHAIN RELEASE` is rejected by the grammar.
- `mariadb/sql/sql_parse.cc` computes `tx_chain` from the explicit chain
  modifier or `@@session.completion_type=CHAIN`, computes `tx_release` from the
  explicit release modifier or `@@session.completion_type=RELEASE`, then calls
  `trans_commit()` or `trans_rollback()`.
- `mariadb/sql/sql_parse.cc` starts a new transaction with `trans_begin(thd)`
  after successful commit or rollback when `tx_chain` is true, and marks the
  connection killed when `tx_release` is true.
- `mariadb/sql/sys_vars.cc` defines `completion_type` values `NO_CHAIN`,
  `CHAIN`, and `RELEASE`. MyLite must not allow session completion defaults to
  silently alter plain `COMMIT` / `ROLLBACK` semantics until it mirrors those
  defaults in direct transaction state.

## Design

Extend the direct `libmylite` transaction-control policy with modifier support
that maps cleanly to the current MyLite storage checkpoint model:

- `START TRANSACTION READ WRITE` starts or restarts a direct MyLite transaction
  exactly like plain `START TRANSACTION`. Repeated `READ WRITE` options in the
  same option list are treated as the same explicit read-write request.
- `START TRANSACTION READ ONLY`, `WITH CONSISTENT SNAPSHOT`, and mixed or
  unknown start options remain unsupported at this slice point. A later
  read-only transaction-control slice accepts the read-only access-mode subset.
- `COMMIT AND CHAIN` commits the current outer MyLite checkpoint and immediately
  opens a new outer checkpoint, regardless of `autocommit` mode.
- `ROLLBACK AND CHAIN` rolls back the current outer MyLite checkpoint and
  immediately opens a new outer checkpoint, regardless of `autocommit` mode.
- `COMMIT AND NO CHAIN`, `ROLLBACK AND NO CHAIN`, and `NO RELEASE` completion
  forms are accepted as explicit no-op modifiers over the current plain
  completion behavior.
- `RELEASE` completion forms remain unsupported because embedded `libmylite`
  has no client connection to disconnect and no server session owner to kill.
- At this slice point, `SET completion_type`, transaction isolation variables,
  transaction read-only variables, `SET TRANSACTION`, XA, prepared
  transaction-start/completion statements, and DDL inside active direct
  transactions remain unsupported.

The implementation stays in first-party `packages/libmylite` SQL policy and
post-execution transaction bookkeeping. No MariaDB grammar or upstream-derived
execution code is changed.

## Affected Subsystems

- `packages/libmylite`: direct transaction-control classification and
  post-execution transaction state.
- Direct SQL policy tests.
- Storage-engine smoke tests for routed durable MyLite row-DML transactions.
- API, storage architecture, compatibility matrix, roadmap, and prior
  transaction-slice documentation.

## Compatibility Impact

Applications that emit common MariaDB/MySQL transaction modifier forms gain a
larger supported direct SQL transaction surface:

- explicit `READ WRITE` transaction starts,
- explicit no-chain/no-release completion modifiers,
- chained commit and rollback.

Compatibility remains partial. MyLite still does not claim read-only
transaction enforcement, consistent snapshots, isolation levels, XA, connection
release semantics, session completion defaults, transactional DDL, handler-level
savepoint hooks, or fully transactional engine flags.

## DDL Metadata Routing Impact

No catalog format changes are introduced. DDL remains rejected while a direct
transaction checkpoint or savepoint is active.

## Single-File And Embedded Lifecycle

No durable sidecar is added. Chained completion closes the current transaction
journal by committing or rolling back it, then opens a new transaction journal
for the next direct transaction over the same primary `.mylite` file.

`RELEASE` remains unsupported because `libmylite` owns an embedded database
handle, not a wire-protocol client connection.

## Public API And File Format

The public C API and file format do not change. Behavior changes are limited to
which SQL strings are accepted by `mylite_exec()` and how MyLite mirrors
MariaDB's accepted direct transaction state.

## Storage-Engine Routing Impact

The behavior applies to routed durable MyLite tables, including requested
`ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` tables that resolve to
MyLite storage. MEMORY/HEAP volatile row transactions remain outside the durable
row-DML transaction claim.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future protocol wrapper should
preserve these semantics when delegating transaction-control SQL to `libmylite`.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is negligible.

## Test And Verification Plan

- Extend direct SQL policy tests to accept supported modifier syntax and reject
  unsupported start options, release forms, completion defaults, isolation
  variables, XA, and prepared transaction-start/completion statements at this
  slice point.
- Add storage-smoke coverage proving:
  - `START TRANSACTION READ WRITE` starts a transaction over routed
    `ENGINE=InnoDB` rows,
  - `COMMIT AND NO CHAIN` commits without keeping a transaction active,
  - `ROLLBACK AND NO CHAIN` rolls back without keeping a transaction active,
  - `COMMIT AND CHAIN` commits prior row DML and leaves a new transaction whose
    later row DML can roll back,
  - `ROLLBACK AND CHAIN` rolls back prior row DML and leaves a new transaction
    whose later row DML can commit.
- Run dev, embedded, storage-smoke, transaction/direct-SQL/prepared-statement
  harness groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Supported modifier forms succeed through direct `mylite_exec()`.
- Chained commit and rollback keep MyLite storage checkpoint state aligned with
  MariaDB's `trans_begin()` follow-up.
- Unsupported modifier forms fail before MariaDB execution with stable MyLite
  transaction-control diagnostics.
- Prepared transaction-start and completion statements remain rejected at this
  slice point; prepared savepoint control remains supported.
- Docs and compatibility matrix describe the bounded support accurately.

## Risks And Unresolved Questions

- A storage checkpoint failure after MariaDB has accepted `COMMIT AND CHAIN` or
  `ROLLBACK AND CHAIN` can leave the embedded session in an error state. This is
  the same I/O-failure class as plain direct commit or rollback after MariaDB
  has accepted the statement.
- Release completion-default support requires an embedded handle-lifecycle
  decision. This slice rejected completion variable changes; later transaction
  SET slices accept the no-chain/default subset and the chained plain
  completion subset.
- Read-only transactions and consistent snapshots need a broader storage and
  isolation design before they can be supported honestly.
