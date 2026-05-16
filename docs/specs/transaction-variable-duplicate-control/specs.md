# Transaction Variable Duplicate Control

## Goal

Allow direct session `SET` lists to assign supported transaction variables more
than once, matching MariaDB's ordered variable-update behavior where later
assignments determine the effective transaction read-only or isolation setup.

## Non-Goals

- Duplicate `SET TRANSACTION ...` access-mode or isolation clauses.
- Duplicate autocommit assignments.
- Global transaction-variable assignments.
- `DEFAULT.variable` transaction-variable syntax.
- Prepared transaction-variable control.
- Real MyLite storage isolation guarantees.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/set_var.cc:733-750` checks the `SET` variable list, rewinds the
  iterator, and updates variables in list order.
- `mariadb/sql/sys_vars.inl:2420-2478` defines the transaction isolation
  variable behavior for global, session, and next-transaction scopes.
- `mariadb/sql/sys_vars.inl:2482-2505` and
  `mariadb/sql/sys_vars.cc:4510-4568` define `transaction_read_only` /
  `tx_read_only`, including active-transaction checks for next-transaction
  updates.
- `mariadb/sql/transaction.cc:45-56` resets one-shot transaction isolation and
  read-only state to the session defaults when a transaction ends.

## Compatibility Impact

This narrows MyLite's conservative transaction-variable policy. Supported
direct assignments to `transaction_read_only`, `tx_read_only`,
`transaction_isolation`, and `tx_isolation` may repeat in a `SET` list. The
final supported read-only assignment controls MyLite's mirrored read-only state.
Isolation assignments remain compatibility setup SQL only; MyLite still does
not claim storage isolation semantics.

## Design

Change only direct `SET` assignment-list policy:

- keep rejecting duplicate `SET TRANSACTION ...` characteristics,
- keep rejecting duplicate autocommit assignments,
- allow repeated supported transaction read-only variable assignments,
- allow repeated supported transaction isolation variable assignments,
- preserve rejection for global scope, `DEFAULT.variable`, invalid read-only
  values, `SET STATEMENT`, prepared forms, and semicolon tails.

After MariaDB accepts the whole statement, MyLite already walks the `SET` list
and applies read-only assignments in order. That sequential application is kept
so the final supported read-only assignment wins while session and
next-transaction scopes retain their existing MyLite behavior.

## File Lifecycle

No file-format, journal, lock, or companion-file behavior changes.

## Embedded Lifecycle And API

No public C API changes are required. The behavior is visible through direct
SQL execution and later transaction read-only enforcement.

## Build, Size, And Dependencies

No dependency or build-profile change.

## Test Plan

- Add direct SQL policy coverage proving duplicate supported transaction
  read-only and isolation variable assignments succeed.
- Keep duplicate `SET TRANSACTION` characteristics, duplicate autocommit,
  global scope, invalid values, `SET STATEMENT`, and semicolon tails rejected.
- Add storage-smoke coverage proving final read-write and final read-only
  transaction variable assignments determine later write policy.
- Run dev, embedded, storage-smoke, compatibility harness, formatting, tidy,
  shell syntax, and whitespace checks.

## Acceptance Criteria

- `SET transaction_read_only=1, tx_read_only=0` succeeds and the next
  transaction is writable.
- `SET tx_read_only=0, transaction_read_only=1` succeeds and the next
  transaction rejects durable MyLite writes.
- Duplicate isolation variable assignments are accepted as compatibility setup
  SQL without claiming storage isolation.
- Previously unsupported scopes and values remain rejected.
