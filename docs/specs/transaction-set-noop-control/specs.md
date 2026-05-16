# Transaction SET No-Op Control

Status note: the later
[Completion Type Chain Control](../completion-type-chain-control/specs.md)
slice accepts session `SET completion_type=CHAIN/1` and mirrors chained plain
`COMMIT` / `ROLLBACK`. Release completion defaults remain unsupported.
The later
[Completion-Type Duplicate Control](../completion-type-duplicate-control/specs.md)
slice accepts duplicate supported `completion_type` assignments with the final
assignment winning.
The later
[Read-Only Transaction Control](../read-only-transaction-control/specs.md)
slice accepts direct read-only transaction access-mode controls and rejects
MyLite storage writes while the read-only characteristic is active.
The later
[Transaction Isolation Control](../transaction-isolation-control/specs.md)
slice accepts direct/session `SET TRANSACTION ISOLATION LEVEL ...` forms as
compatibility setup SQL without claiming storage isolation semantics.

## Problem

MyLite supports bounded direct row-DML transactions, but it still rejects every
`SET TRANSACTION` statement and every `completion_type` assignment. Some
MySQL/MariaDB clients issue explicit read-write transaction setup or reset the
transaction completion type to its default no-chain behavior. Those forms do
not require new MyLite storage semantics, but rejecting them is stricter than
MariaDB and blocks harmless session setup.

This slice accepts only direct read-write/no-chain controls that are no-ops for
MyLite's current row-DML transaction scope. At this slice point, read-only
access modes, isolation controls, chain/release completion defaults, global
changes, `SET STATEMENT`, prepared forms, XA, and DDL inside active direct
transactions remained unsupported.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `SET TRANSACTION` through the normal
  `SQLCOM_SET_OPTION` grammar. `SET TRANSACTION READ WRITE` records
  `transaction_read_only = false`; `READ ONLY` records `true`; isolation
  clauses record `transaction_isolation`.
- The same grammar accepts `SET SESSION TRANSACTION ...` and
  `SET LOCAL TRANSACTION ...` through `option_type`, while `SET GLOBAL
  TRANSACTION ...` targets global defaults.
- `mariadb/sql/sys_vars.cc:Sys_completion_type` defines the session
  `completion_type` enum with values `NO_CHAIN`, `CHAIN`, and `RELEASE`, and
  `DEFAULT(0)`.
- `mariadb/sql/sql_parse.cc` computes plain `COMMIT` and `ROLLBACK` chaining
  from explicit modifiers or `thd->variables.completion_type == 1`, and
  computes release from explicit modifiers or `completion_type == 2`.
- `mariadb/sql/transaction.cc:trans_reset_one_shot_chistics()` resets
  one-shot transaction isolation and access mode after a transaction ends.
- `mariadb/sql/set_var.cc:sys_var::update()` treats
  `transaction_isolation` and `transaction_read_only` as variables that can
  exist as global, session, or one-shot values, which is broader than MyLite's
  current supported transaction scope.

## Design

Add a direct `SET` transaction-control classifier that runs before the generic
transaction-variable rejection:

- Accept direct `SET TRANSACTION READ WRITE`.
- Accept direct `SET SESSION TRANSACTION READ WRITE` and
  `SET LOCAL TRANSACTION READ WRITE`, because they select the current default
  MyLite behavior for future transactions.
- Accept direct session `completion_type` assignments to `NO_CHAIN`, `0`, or
  `DEFAULT`, including `@@completion_type`, `@@session.completion_type`,
  `SESSION completion_type`, and `LOCAL completion_type`.
- Allow a supported no-chain `completion_type` assignment to appear in the same
  direct `SET` list as ordinary non-transaction assignments or one supported
  session autocommit assignment.
- Return a MyLite no-op transaction-control kind for direct no-op controls so
  prepared statements can continue rejecting them before MariaDB prepare.

Reject before MariaDB execution:

- `SET GLOBAL TRANSACTION ...` and global `completion_type` assignments,
  because they mutate process-global defaults.
- `SET TRANSACTION READ ONLY` and all isolation clauses. A later read-only
  transaction-control slice accepts the read-only access-mode subset.
- `SET completion_type=CHAIN`, `SET completion_type=RELEASE`, numeric
  chain/release values, duplicate `completion_type` assignments, and
  semicolon-chained no-op controls.
- `SET STATEMENT completion_type=... FOR ...`, even when the value is
  `NO_CHAIN`.
- Prepared forms of accepted direct controls.

Execution does not change MyLite storage state for the no-op kind. MariaDB
still executes the direct statement first, so malformed syntax or unsupported
MariaDB values fail through normal MariaDB diagnostics without changing MyLite
transaction state.

## Affected Subsystems

- `packages/libmylite`: direct SQL transaction-control parsing.
- Embedded direct and prepared SQL policy tests.
- Storage-engine transaction smoke tests over routed MyLite tables.
- API, storage architecture, compatibility matrix, roadmap, and transaction
  spec docs.

## Compatibility Impact

Applications can issue explicit read-write transaction defaults and no-chain
completion defaults without leaving MyLite's supported transaction semantics.
At this slice point, MyLite still did not claim read-only transactions,
isolation levels, chained completion defaults, release completion defaults, XA,
transactional DDL, or fully transactional handler flags. Later
transaction-control slices add chained completion defaults and bounded
read-only access mode.

## DDL Metadata Routing Impact

No DDL metadata behavior changes. DDL remains rejected while a direct MyLite
transaction checkpoint is active.

## Single-File And Embedded Lifecycle

No file-format, journal, lock, or companion-file behavior changes. Accepted
no-op controls do not open, commit, roll back, or release storage checkpoints.

## Public API And File Format

The public C API and primary `.mylite` file format do not change. The behavior
is exposed through direct SQL execution.

## Storage-Engine Routing Impact

The behavior applies to routed durable MyLite row storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. It does not add native InnoDB
isolation, read-only, release, or handler flag semantics.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. Future protocol adapters should
delegate these direct no-op controls to the core library and keep unsupported
transaction defaults rejected unless the core grows matching semantics.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to parser policy and
tests.

## Test And Verification Plan

- Extend direct SQL policy tests to accept read-write `SET TRANSACTION` forms
  and no-chain/default `completion_type` forms.
- Keep read-only/isolation `SET TRANSACTION`, global and duplicate
  `completion_type`, chain/release completion defaults, `SET STATEMENT`,
  prepared forms, and semicolon-chained forms rejected.
- Add storage-smoke coverage proving:
  - `SET TRANSACTION READ WRITE` before `BEGIN` still allows a routed
    `ENGINE=InnoDB` row-DML transaction to commit,
  - `SET completion_type=NO_CHAIN` keeps plain `COMMIT` as no-chain behavior
    for routed MyLite row-DML transactions,
  - supported no-chain completion assignment can coexist with supported direct
    autocommit assignment without changing the autocommit storage semantics.
- Run dev, embedded, storage-smoke, transaction/direct-SQL/prepared-statement
  harness groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct read-write `SET TRANSACTION` controls are accepted only for local or
  session/default scope.
- Direct session `completion_type=NO_CHAIN/0/DEFAULT` controls are accepted
  without changing MyLite storage transaction state.
- Prepared forms remain explicit transaction-control policy failures.
- Read-only, isolation, chain/release, global, duplicate, statement-scoped, and
  chained-statement forms remain explicit unsupported transaction-control
  surfaces.
- Docs and compatibility tables describe the supported no-op subset without
  claiming isolation or release semantics.

## Risks And Unresolved Questions

- MyLite still uses conservative SQL scanning rather than MariaDB's parsed
  `set_var` list. Suspicious transaction-control targets remain rejected.
- Supporting `completion_type=CHAIN` would require MyLite to mirror that
  session variable and alter later plain `COMMIT` / `ROLLBACK` behavior.
- Supporting read-only or isolation settings needs a broader storage and
  concurrency design before MyLite can claim those semantics.
