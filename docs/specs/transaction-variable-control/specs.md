# Transaction Variable Control

## Problem

MyLite accepts direct/session `SET TRANSACTION` access-mode and isolation-level
controls, but still rejects the equivalent MariaDB transaction system-variable
assignments. Some MySQL/MariaDB clients configure sessions with
`transaction_read_only`, `tx_read_only`, `transaction_isolation`, or
`tx_isolation` instead of the `SET TRANSACTION ...` syntax.

This slice accepts bounded transaction variable assignments after MariaDB
validates them. MyLite mirrors only the read-only access-mode state it already
owns; isolation variables remain compatibility setup SQL until storage locking,
WAL/checkpoints, and transactional engine flags can prove real isolation.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:17035-17238` parses ordinary `SET` variable
  assignments with default `OPT_SESSION` scope, while `@@` system-variable
  assignments use `opt_var_ident_type`; the unqualified `@@variable` form maps
  to `OPT_DEFAULT`.
- `mariadb/sql/sql_yacc.yy:17460-17521` maps `SET TRANSACTION` access modes to
  `transaction_read_only` and isolation levels to `transaction_isolation`.
- `mariadb/sql/sys_vars.inl:2426-2474` documents
  `Sys_var_tx_isolation`: global/session forms set defaults, and
  `OPT_DEFAULT` sets the next transaction only.
- `mariadb/sql/sys_vars.inl:2483-2504` and
  `mariadb/sql/sys_vars.cc:4511-4568` document `Sys_var_tx_read_only`,
  including the same session/default split and active-transaction checks for
  the one-shot next-transaction form.
- `mariadb/sql/set_var.cc:199-231` updates global variables separately from
  session variables and explicitly excludes `transaction_isolation` and
  `transaction_read_only` one-shot updates from ordinary session tracking.
- `mariadb/sql/transaction.cc:48-58` resets one-shot transaction
  characteristics to session values after transaction completion.

## Design

- Accept direct SQL assignments for:
  - `SET transaction_read_only=0/1`
  - `SET tx_read_only=0/1`
  - `SET SESSION` / `SET LOCAL` read-only aliases
  - `SET @@transaction_read_only=0/1` and `SET @@tx_read_only=0/1` as
    MariaDB-style one-shot next-transaction controls
  - `SET @@session.transaction_read_only=0/1` and local/session aliases
  - `SET transaction_isolation=...` and `SET tx_isolation=...` with the same
    session/local/`@@` scope handling.
- Keep `GLOBAL` and `@@global` transaction variable assignments rejected in the
  public MyLite SQL API.
- Keep `DEFAULT.variable` transaction-variable assignment syntax rejected until
  MyLite has a reason to mirror it.
- Accept read-only boolean values as `0` / `OFF` / `FALSE` and `1` / `ON` /
  `TRUE`; broader expression-valued read-only assignments remain out of scope.
- Keep duplicate read-only or duplicate isolation variable assignments rejected
  in one direct `SET` statement to avoid hidden last-writer-wins state.
- Keep `SET STATEMENT ... FOR ...` transaction-variable assignments rejected.
- Keep prepared transaction-variable controls rejected before MariaDB prepare.
- Let MariaDB validate isolation values. MyLite records no storage isolation
  state yet.
- Mirror read-only variable assignments into the existing MyLite
  read-only/read-write state only after MariaDB executes the statement
  successfully.

## Affected Subsystems

- `packages/libmylite`: transaction-control SQL classification and read-only
  state mirroring.
- Direct SQL, prepared statement, and storage-engine transaction tests.
- API, storage architecture, compatibility matrix, roadmap, and related slice
  specs.

## Compatibility Impact

Applications can now use common transaction variable setup SQL without tripping
MyLite's transaction policy gate. Session read-only variables affect the
session default for future MyLite transactions; `@@transaction_read_only`
affects only the next transaction. Isolation variables are accepted as
compatibility setup SQL, not as implemented MyLite storage isolation.

## DDL Metadata Routing Impact

No DDL metadata changes.

## Single-File And Embedded Lifecycle

No file-format, journal, lock, or companion-file behavior changes. Transaction
variable assignments do not open or close MyLite storage checkpoints.

## Public API And File Format

No C API or `.mylite` file-format changes. The behavior is exposed through
`mylite_exec()` accepting additional direct `SET` statements.

## Storage-Engine Routing Impact

The accepted variables apply to routed MyLite tables, including `ENGINE=InnoDB`
requests that resolve to MyLite. Only read-only access mode is mirrored into
current MyLite transaction policy; native InnoDB isolation, consistent
snapshots, gap locks, lock waits, and transactional engine flags remain planned.

## Wire Protocol Or Integration Impact

No wire-protocol package changes. Future protocol adapters should delegate
these controls to the core library.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to transaction
assignment parser branches and tests.

## Test And Verification Plan

- Extend direct SQL transaction policy tests to accept session/local and `@@`
  transaction read-only and isolation variable assignments.
- Cover session read-only defaults, one-shot `@@transaction_read_only`, and
  mixed isolation setup with existing supported `SET` assignments.
- Keep global variables, duplicate transaction characteristics, invalid
  read-only values, semicolon-chained forms, and `SET STATEMENT` forms
  rejected.
- Extend prepared-statement policy tests to keep prepared transaction variable
  controls rejected.
- Add storage-smoke coverage proving read-only variable assignments reject
  MyLite storage writes and reset correctly.
- Run dev, embedded, storage-smoke, transaction/direct-SQL/prepared-statement
  harness groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct transaction read-only and isolation variable assignments are accepted
  after MariaDB accepts the SQL.
- Read-only variable assignments preserve the existing MyLite
  read-only/read-write state behavior for session defaults and one-shot next
  transactions.
- Global controls, duplicate characteristics, invalid read-only values,
  prepared transaction-control statements, and semicolon-chained forms remain
  rejected.
- Docs describe isolation variables as accepted compatibility setup SQL, not
  implemented storage isolation.

## Risks And Unresolved Questions

- Accepting isolation variables may let applications assume stronger storage
  isolation than MyLite currently implements. The compatibility matrix and API
  docs must keep that limitation explicit.
- Broader expression-valued read-only assignments stay out of scope until there
  is a need to mirror MariaDB's full system-variable expression evaluation.
