# Completion-Type Duplicate Control

## Goal

Allow direct session `SET completion_type=...` statements to contain multiple
supported `completion_type` assignments, using MariaDB's normal left-to-right
`SET` behavior where the final assignment determines the session default.

## Non-Goals

- `completion_type=RELEASE/2`.
- Global `completion_type` assignments.
- Prepared transaction-completion statements.
- Duplicate autocommit assignments at this slice point or duplicate
  transaction characteristic assignments. The later
  [Autocommit Duplicate Control](../autocommit-duplicate-control/specs.md)
  slice supports duplicate supported session autocommit assignments.
- Connection release semantics, XA, or broader transaction completion changes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:937-941` defines `completion_type` as a session
  enum with `NO_CHAIN`, `CHAIN`, and `RELEASE` values.
- `mariadb/sql/set_var.cc:733-748` checks the `SET` variable list, rewinds the
  same iterator, and then updates variables in list order, so duplicate session
  assignments leave the last value in effect.
- `mariadb/sql/sql_yacc.yy:18284-18317` parses explicit `AND [NO] CHAIN` and
  `[NO] RELEASE` completion modifiers on `COMMIT` and `ROLLBACK`.
- `mariadb/sql/transaction.cc:351-355` documents that `completion_type` does
  not affect implicit commit; MyLite's direct explicit `COMMIT` / `ROLLBACK`
  path is the relevant surface.

## Compatibility Impact

This narrows a MyLite-only rejection for direct `SET completion_type` lists.
`NO_CHAIN`, `0`, `DEFAULT`, `CHAIN`, and `1` remain the only supported values.
`RELEASE` remains unsupported because the embedded core does not expose
connection-release semantics.

## Design

Keep the existing parser and policy boundary, but change duplicate
`completion_type` handling:

- do not reject a second supported session `completion_type` assignment,
- remember the final supported value in the statement,
- keep rejecting unsupported values, global scope, `SET STATEMENT`, and
  semicolon tails,
- keep duplicate autocommit unsupported at this slice point and duplicate
  transaction characteristic assignments unsupported.

The execution path still lets MariaDB apply the `SET` first, then mirrors the
final supported completion default into `mylite_db::completion_type_chain`.

## File Lifecycle

No file-format or companion-file behavior changes.

## Embedded Lifecycle And API

No public C API changes are required. The behavior is visible through direct
SQL execution.

## Build, Size, And Dependencies

No dependency or build-profile change.

## Test Plan

- Add direct SQL policy coverage proving duplicate supported
  `completion_type` assignments succeed.
- Prove the final assignment wins for both final `NO_CHAIN` and final `CHAIN`
  by observing later plain `COMMIT` behavior.
- Keep existing failures for global scope, `RELEASE`, `SET STATEMENT`, and
  semicolon tails.
- Run dev, embedded, storage-smoke, compatibility harness, formatting, tidy,
  shell syntax, and whitespace checks.

## Acceptance Criteria

- `SET completion_type=CHAIN, completion_type=NO_CHAIN` succeeds and later
  plain `COMMIT` does not chain.
- `SET completion_type=NO_CHAIN, completion_type=CHAIN` succeeds and later
  plain `COMMIT` chains.
- Unsupported completion-type scopes and values remain rejected.
