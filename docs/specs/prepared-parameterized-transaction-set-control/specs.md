# Prepared Parameterized Transaction SET Control

## Problem

MyLite now supports prepared transaction lifecycle SQL and prepared literal
transaction-related `SET` controls, but it still rejects prepared values such
as `SET autocommit=?` at MyLite prepare time. That blocks application and
driver code that prepares session setup SQL while still expecting MyLite's
bounded row-DML transaction mirror to stay in sync with MariaDB.

## Scope

Allow prepared transaction-related `SET` assignments where the assignment value
is a single parameter marker:

- `SET autocommit=?` and `SET @@session.autocommit=?`,
- `SET completion_type=?` and `SET @@session.completion_type=?`,
- `SET transaction_read_only=?`, `SET tx_read_only=?`,
  `SET @@transaction_read_only=?`, and `SET @@tx_read_only=?`,
- `SET transaction_isolation=?` and `SET tx_isolation=?` as compatibility setup
  SQL without claiming storage isolation.

The supported forms may appear in ordinary `SET` assignment lists with other
non-transaction assignments. MyLite must preserve assignment order for
autocommit because `autocommit=1` commits the current bounded transaction and a
later `autocommit=0` starts the next one.

## Non-Goals

- Parameterized `SET TRANSACTION ...` grammar. MariaDB's transaction
  characteristics grammar is keyword-based rather than expression-based.
- Parameter expressions such as `SET autocommit=? + 0`.
- `SET STATEMENT ... FOR ...`.
- Global autocommit, completion-type, or transaction variable assignments.
- `completion_type=RELEASE/2`.
- `DEFAULT` supplied through a bound parameter. `DEFAULT` remains supported as
  SQL syntax, not as a parameter value.
- Real storage isolation, durable transactional DDL, XA, or handler-level
  transactional engine flags.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:17297-17305` parses `@@... =` system-variable
  assignments through `set_expr_or_default`; `17538-17550` maps that rule to
  ordinary expressions, which include prepared parameter markers.
- `mariadb/sql/sql_yacc.yy:17460-17514` parses `SET TRANSACTION` access modes
  and isolation levels as fixed keywords, not value expressions.
- `mariadb/sql/sql_prepare.cc:23-67` documents that prepared statements parse
  parameter markers at prepare time and assign parameter values during execute.
- `mariadb/sql/sql_prepare.cc:5070-5098` executes prepared statements through
  `mysql_execute_command(thd, true)`, so prepared `SET` statements reach the
  same `set_var` execution path after parameter binding.
- `mariadb/sql/set_var.cc:778-869` checks system-variable values during
  execution and updates the variable only after the value is available.
- `mariadb/sql/sys_vars.cc:4805-4870` implements `autocommit` session updates,
  including commit-on-enable and disable-autocommit behavior.
- `mariadb/sql/sys_vars.cc:4522-4609` defines transaction isolation and
  transaction read-only system variables and their active-transaction checks.

## Design

Extend MyLite's transaction-control classifier with a prepared-only
parameterized `SET` mode:

- direct `mylite_exec()` keeps rejecting transaction-control parameter markers
  as unsupported policy SQL;
- `mylite_prepare()` allows supported single-marker transaction-related `SET`
  assignments to reach MariaDB's prepared statement API;
- unsupported parameterized transaction-related targets remain MyLite policy
  failures before MariaDB prepare.

During `mylite_step()`:

- after MyLite binds parameters and before MariaDB execution, resolve the
  supported bound parameter values into concrete transaction-control actions;
- reject unsupported bound values, including `NULL`, `completion_type=RELEASE`
  or `2`, and bound `DEFAULT`, before MariaDB can mutate session state;
- execute the prepared statement through MariaDB;
- after successful execution, replay the resolved MyLite transaction controls
  in SQL assignment order.

The ordered replay is necessary for mixed lists such as
`SET autocommit=?, autocommit=?`. Literal assignments keep the existing direct
parser behavior; only statements that include supported single-marker
transaction values use the resolved replay path.

## Affected Subsystems

- `packages/libmylite`: transaction-control classification, prepared
  parameter validation, and post-execute transaction-state mirroring.
- Embedded prepared-statement diagnostics.
- Storage-engine transaction smoke coverage.
- API, storage, compatibility, roadmap, and historical slice documentation.

## Compatibility Impact

Prepared parameterized transaction setup SQL becomes supported for the bounded
row-DML transaction scope that MyLite already documents. The behavior remains
partial: isolation assignments are accepted as compatibility setup SQL, not as
a storage-isolation guarantee, and MyLite still rejects server/global,
`RELEASE`, expression-valued, and `SET STATEMENT` forms.

## DDL Metadata Routing Impact

No DDL metadata format change.

## Single-File And Embedded Lifecycle

No file-format change. Parameterized `autocommit=0` opens the same outer
MyLite transaction checkpoint and transaction journal as literal prepared
autocommit control. Parameterized `autocommit=1` commits and removes that
journal. Invalid bound values are rejected before MariaDB changes session
state.

## Public API And File Format

No public C API or `.mylite` file-format change. Existing
`mylite_prepare()`, `mylite_bind_*()`, `mylite_step()`, `mylite_reset()`, and
`mylite_finalize()` paths expose the behavior.

## Storage-Engine Routing Impact

The support applies to routed durable MyLite tables, including `ENGINE=InnoDB`
requests that resolve to MyLite. It does not add native InnoDB transactional
semantics or handler-level transactional flags.

## Wire Protocol Or Integration Impact

No wire-protocol package change. Future protocol adapters should delegate
prepared transaction `SET` execution to this core behavior.

## Binary-Size And Dependency Impact

No dependency or build-profile change. The implementation adds only local SQL
classification and bound-value interpretation helpers.

## Test And Verification Plan

- Prepared-statement diagnostics:
  - `SET autocommit=?`, `SET transaction_read_only=?`,
    `SET transaction_isolation=?`, and `SET completion_type=?` now prepare and
    execute with supported values.
  - Unsupported parameterized `SET STATEMENT`, global forms, expression values,
    `NULL`, and `completion_type=2/RELEASE` fail with stable policy or MariaDB
    diagnostics as appropriate.
- Storage transaction coverage:
  - parameterized `autocommit=0` rolls back routed row DML;
  - parameterized `autocommit=1` commits routed row DML;
  - mixed parameterized autocommit lists preserve assignment order;
  - parameterized completion-type controls affect later plain completion;
  - parameterized transaction read-only controls reject durable writes and can
    be reset to read-write;
  - parameterized isolation assignments execute without claiming isolation.
- Run targeted embedded and storage-engine tests plus formatting, shell syntax,
  `.reject`, and whitespace checks.

## Acceptance Criteria

- Prepared single-marker transaction-related `SET` values succeed for the
  supported bounded forms.
- MyLite rejects invalid bound values before MariaDB mutates session state.
- MyLite transaction checkpoints mirror parameterized autocommit transitions in
  SQL assignment order.
- Docs and compatibility tables no longer list parameterized
  transaction-control `SET` values as wholly unsupported.

## Risks And Open Questions

- MyLite still uses a conservative first-party SQL scanner rather than
  MariaDB's parsed `LEX` tree for policy decisions.
- Supporting expression-valued transaction parameters would require evaluating
  MariaDB's accepted value without letting unsupported values mutate session
  state first; the later
  [Transaction Control Rejection Matrix](../transaction-control-rejection-matrix/specs.md)
  slice keeps these forms explicitly rejected until applications prove the
  need.
