# Ownerless Schema Option-Order Recovery

## Problem

Ownerless schema crash coverage already proves separate `ALTER DATABASE`
default charset/collation rewrites and schema-comment rewrites. The remaining
schema option-order gap is a single native `db.opt` rewrite that carries both
option classes in one `ALTER DATABASE` statement, especially when the comment
option appears before the default charset/collation options.

This slice adds deterministic live-peer recovery evidence for that combined
rewrite without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:5629` defines `create_database_options` as a
  repeated list of `create_database_option` entries, so schema options can be
  supplied together in order.
- `mariadb/sql/sql_yacc.yy:5634` accepts default collation, default charset,
  and `COMMENT [=] 'literal'` as schema options.
- `mariadb/sql/sql_yacc.yy:7471` routes `ALTER DATABASE ident_or_empty` through
  `create_database_options`; `mariadb/sql/sql_yacc.yy:7487` also preserves the
  current-schema `ALTER DATABASE COMMENT ...` spelling with additional
  optional schema options.
- `mariadb/sql/sql_db.cc:482` loads the existing `db.opt` values for
  `ALTER DATABASE` when a statement supplies only one option class.
- `mariadb/sql/sql_db.cc:896` implements `mysql_alter_db_internal()` by locking
  the schema and rewriting the native `db.opt` file through `write_db_opt()`;
  `mariadb/sql/sql_db.cc:507` serializes both default charset/collation and
  optional schema comment into that same file.

## Scope And Non-Goals

In scope:

- A hook-build selector that kills `ALTER DATABASE <schema> COMMENT = ...
  DEFAULT CHARACTER SET ... COLLATE ...` after MariaDB rewrites native
  `db.opt` but before ownerless dictionary finish.
- Live-peer ownerless recovery while another process remains open.
- Verification that recovered `INFORMATION_SCHEMA.SCHEMATA` contains both the
  new comment and new default charset/collation.
- Verification that pre-alter table column metadata keeps the old collation,
  while a post-recovery table inherits the recovered default collation.
- Final ownerless/native reopen and forced `.shm` rebuild checks.

Out of scope:

- Invalid schema-option cleanup.
- Randomized external DDL stress.
- Current-schema `ALTER DATABASE COMMENT ...` with appended options; separate
  current-schema comment/default crash coverage already exists.
- Production behavior changes.

## Design

Add the hook selector `dictionary-schema-option-order-crash` to
`mylite_ownerless_cross_process_sql_test`.

The selector creates `ownerless_schema_option_order_crash` with `latin1`
defaults and an initial schema comment, then creates one InnoDB table whose
string column records the pre-alter default collation. The child writer runs:

```sql
ALTER DATABASE ownerless_schema_option_order_crash
  COMMENT = 'ownerless recovered option order'
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci
```

with the existing `dictionary-before-finish` fault hook armed. The parent holds
another ownerless peer open while recovery runs, verifies the native
file-operation checkpoint-needed marker stays clear, checks recovered metadata,
then releases the peer and creates a new table that must inherit the recovered
`utf8mb4` default.

## Compatibility Impact

No SQL semantics change. The slice records that a supported MariaDB
multi-option `ALTER DATABASE` rewrite is recoverable in ownerless mode and
that schema defaults/comments remain visible through standard
`INFORMATION_SCHEMA.SCHEMATA` and column metadata queries.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises MariaDB's native `db.opt`
rewrite inside the MyLite database directory and proves ownerless recovery
publishes the recovered state while a peer remains live.

## Native Storage Impact

No native storage format changes. The covered path is metadata-only schema
state; the native file-operation marker must remain clear.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds hook-test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run the focused hook selector:
  `dictionary-schema-option-order-crash`.
- Run the adjacent schema dictionary hook subset.
- Build `mylite_ownerless_cross_process_sql_test` in `embedded-prod`.
- Run the production embedded ownerless schema/default/comment subset where
  applicable.
- Run CI production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- A killed combined schema-option `ALTER DATABASE` writer recovers while
  another ownerless peer remains live.
- Recovered metadata contains both the new schema comment and new default
  charset/collation.
- Existing tables keep their pre-alter column collation.
- Tables created after recovery inherit the recovered default collation.
- Final state survives ownerless reopen, native read/write reopen, and forced
  shared-memory rebuild.

## Risks And Follow-Up

- Invalid schema-option cleanup remains planned.
- Randomized external schema-DDL oracles remain part of the broader DDL
  lifecycle completion gate.
