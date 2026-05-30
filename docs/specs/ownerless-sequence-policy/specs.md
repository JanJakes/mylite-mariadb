# Ownerless Sequence Policy

## Problem

MariaDB sequences are table-backed schema objects, and sequence value access is
not a pure read: `NEXT VALUE` / `NEXTVAL()` advances sequence state. MyLite
currently covers simple exclusive embedded sequence behavior, but ownerless
read/write mode is designed around coordinated InnoDB user-table writes. It
does not yet coordinate sequence-table metadata, sequence cache state, or
cross-process sequence value advancement.

Ownerless mode should reject sequence DDL and sequence value access before
MariaDB mutates sequence state.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation for sequences
  (`https://mariadb.com/kb/en/sequence-overview/`) describes sequences as
  objects that generate numeric values and support `NEXT VALUE FOR`.
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_CREATE_SEQUENCE`,
  `SQLCOM_ALTER_SEQUENCE`, and `SQLCOM_DROP_SEQUENCE` as data-changing DDL.
- `mariadb/sql/sql_table.cc` handles `CREATE SEQUENCE` through normal table
  creation with `create_info->sequence`, then calls `sequence_insert()` to
  initialize the sequence row.
- `mariadb/sql/sql_table.cc:mysql_rm_table()` has explicit `drop_sequence`
  handling and validates that the dropped object is a sequence table.
- `mariadb/sql/sql_lex.h` and `mariadb/sql/sql_lex.cc` create sequence value
  expression items for `NEXT VALUE`, `PREVIOUS VALUE`, `NEXTVAL()`,
  `LASTVAL()`, and `SETVAL()`.
- `mariadb/sql/item_func.cc:Item_func_nextval::val_int()` and
  `Item_func_setval::val_int()` operate on the opened sequence table and update
  sequence state.
- MyLite ownerless mode currently rejects non-InnoDB table engines at the SQL
  policy layer, but sequences are a separate command/value-access surface and
  need an explicit ownerless policy boundary.

## Scope And Non-Goals

- Reject ownerless read/write `CREATE`, `ALTER`, and `DROP SEQUENCE`.
- Reject ownerless sequence value access through `NEXT VALUE FOR`,
  `PREVIOUS VALUE FOR`, `NEXTVAL()`, `LASTVAL()`, and `SETVAL()`.
- Verify ordinary exclusive embedded sequence behavior remains usable outside
  ownerless mode.
- Verify ownerless rejection does not create the rejected sequence or advance an
  existing exclusive-created sequence.
- Do not add ownerless sequence support, sequence cache coordination,
  cross-process monotonic sequence allocation, sequence DDL recovery, or
  external oracle stress.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.

## Design

- Add an ownerless-only SQL policy predicate in `packages/libmylite/src/database.cc`.
- Tokenize through the existing SQL policy tokenizer.
- Reject top-level sequence DDL when the DDL target is `SEQUENCE`.
- Reject sequence value syntax by scanning raw tokens for:
  - `NEXT VALUE FOR`,
  - `PREVIOUS VALUE FOR`,
  - `NEXTVAL(`,
  - `LASTVAL(`,
  - `SETVAL(`.
- Return a MyLite policy error before MariaDB prepares or executes the
  sequence statement.
- Add a focused `sequence-policy` selector in
  `mylite_ownerless_cross_process_sql_test`.

## Compatibility Impact

Ownerless read/write mode explicitly does not support sequence SQL. This keeps
ordinary exclusive embedded sequence support intact while making the ownerless
unsupported surface deterministic instead of accidental.

Future ownerless sequence support needs a separate design for sequence-table
engine choice, sequence cache durability, cross-process monotonic allocation,
and DDL/recovery behavior.

## Directory And Lifecycle Impact

The policy prevents ownerless mode from creating, dropping, or mutating
sequence-backed table state inside the MyLite database directory. It adds no
new files and verifies rejected sequence objects remain absent across
ownerless/native reopen before and after forced shared-memory rebuild.

## Native Storage Impact

No native storage format changes. The slice is a policy guard around a
non-InnoDB-compatible sequence surface for ownerless mode.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `sequence-policy` selector.
- Build and run the focused `sequence-policy` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage or focused reruns if the
  known intermittent InnoDB log-header checksum abort appears.
- Run the ownerless stress preset, `format-check`, `git diff --check`, and
  cached diff checks before commit.

## Acceptance Criteria

- Ownerless sequence DDL returns a MyLite policy error without creating a
  sequence object.
- Ownerless sequence value access returns a MyLite policy error without
  advancing an existing sequence.
- Ordinary exclusive embedded sequence creation, value access, and drop remain
  usable.
- Final sequence absence survives ownerless/native reopen before and after
  forced `.shm` rebuild.
- Compatibility docs mark ownerless sequence SQL unsupported while retaining
  ordinary exclusive sequence coverage.

## Risks And Follow-Up

- Real ownerless sequence support will need cross-process value allocation and
  recovery semantics rather than only dictionary refresh.
- External randomized stress remains necessary before claiming broad ownerless
  SQL/application compatibility.
