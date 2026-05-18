# Transaction Control Rejection Matrix

## Goal

Make MyLite's remaining transaction-control policy boundary explicit for
forms that MariaDB can parse but MyLite cannot yet mirror safely in the
embedded single-file runtime.

## Scope

Add direct and prepared storage-smoke coverage proving these forms fail with
stable MyLite policy diagnostics before MariaDB mutates session state:

- direct transaction-control parameter markers such as `SET autocommit=?`;
- expression-valued direct controls such as `SET autocommit=1+0`;
- global autocommit, `completion_type`, and transaction-variable assignments;
- global or expression-valued parameterized transaction-control `SET` forms;
- duplicate `SET TRANSACTION` access-mode or isolation characteristics.

## Non-Goals

- Broader transaction support, durable transactional DDL, XA, release
  completion, release completion-type defaults, or real storage isolation.
- Parameter expressions for transaction controls.
- Global transaction-control state in the embedded core.
- Parser refactoring beyond coverage for the existing conservative policy.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:17042-17099` parses `SET TRANSACTION` and scoped
  `SET` option lists.
- `mariadb/sql/sql_yacc.yy:17229-17330` parses system-variable assignments,
  including `@@global...` forms, through expression/default values.
- `mariadb/sql/sql_yacc.yy:17460-17500` limits `SET TRANSACTION`
  characteristics to one access-mode clause, one isolation clause, or one of
  each.
- `mariadb/sql/set_var.cc:733-756` checks every `SET` variable before rewinding
  and applying updates in list order.
- `mariadb/sql/set_var.cc:778-851` validates variable scope and expression
  values, including prepared light checks.
- `mariadb/sql/sys_vars.cc:4522-4609` defines transaction isolation and
  transaction read-only checks.
- `mariadb/sql/sys_vars.cc:4805-4870` implements `autocommit`, including
  global and session update behavior.
- `packages/libmylite/src/database.cc:1513-1518` rejects unsupported direct
  transaction controls before MariaDB execution.
- `packages/libmylite/src/database.cc:1696-1703` rejects unsupported prepared
  transaction controls before MariaDB prepare.
- `packages/libmylite/src/database.cc:4249-4300` rejects global or duplicate
  `SET TRANSACTION` characteristics in MyLite's policy scanner.
- `packages/libmylite/src/database.cc:4303-4408` rejects semicolon tails,
  unsupported transaction assignments, and unsupported parameterized control
  shapes.
- `packages/libmylite/src/database.cc:4411-4605` keeps global autocommit,
  global `completion_type`, global transaction variables, unsupported values,
  and unsupported parameter-expression forms outside the supported transaction
  control set.

## Design

Keep the existing conservative policy scanner and close the one discovered
hole: direct `transaction_isolation=?` and prepared expression-valued
transaction-isolation controls must be rejected by MyLite rather than leaking
to MariaDB validation.

Direct SQL uses `assert_transaction_control_exec_fails()` so the failure must
be first-party MyLite policy error `HY000` with no MariaDB errno. Prepared SQL
uses a prepare-time policy assertion for forms that must not reach MariaDB
prepare, and the existing step-time policy assertions continue to cover invalid
bound values such as bound `DEFAULT`, `RELEASE`, and `NULL`.

## Compatibility Impact

The supported transaction surface is unchanged. MyLite continues to support
bounded direct/prepared row-DML transactions, supported session
`autocommit`, `completion_type`, `SET TRANSACTION`, transaction-variable
assignments, and savepoint rollback/release over routed durable tables.

The newly covered failures keep unsupported server/global and expression
forms explicit rather than accidental. This matters because `ENGINE=InnoDB`
resolves to MyLite storage, so accepting a server-style or expression-valued
transaction control would imply state that the embedded MyLite mirror cannot
currently prove.

## File Lifecycle

No file-format, journal, lock, or companion-file behavior changes.

## Embedded Lifecycle And API

No public C API changes. `mylite_exec()` and `mylite_prepare()` continue to
return `MYLITE_ERROR`, SQLSTATE `HY000`, no MariaDB errno, and stable
transaction-control diagnostics for these policy failures.

## Test Plan

- Add direct storage-smoke coverage for transaction-control parameter markers,
  expression-valued controls, global scoped assignments, and duplicate
  `SET TRANSACTION` characteristics.
- Add prepared prepare-time policy coverage for global parameterized controls,
  expression-valued parameterized controls, and duplicate `SET TRANSACTION`
  characteristics.
- Keep existing prepared step-time policy coverage for invalid bound values.
- Run the focused storage-smoke binary, the transaction compatibility harness
  group, shell syntax checks, reject-file cleanup checks, and whitespace
  checks.

## Acceptance Criteria

- Direct parameter-marker and expression-valued transaction controls fail
  before MariaDB execution.
- Global direct and prepared parameterized transaction controls fail before
  session state changes.
- Duplicate `SET TRANSACTION` characteristics fail before MariaDB execution or
  prepare.
- Existing supported bounded transaction controls still pass.

## Risks And Open Questions

- MyLite still relies on a conservative first-party scanner instead of
  MariaDB's parsed `LEX` tree for this boundary.
- If real applications require parameter expressions or global compatibility
  no-ops, a later slice should either evaluate them safely from MariaDB's
  parsed state or deliberately map them to documented no-op behavior.
