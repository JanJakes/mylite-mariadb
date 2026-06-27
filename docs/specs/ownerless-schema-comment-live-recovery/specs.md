# Ownerless Schema Comment Live Recovery

## Problem

Ownerless schema crash coverage proves representative schema create, default
charset/collation alter, idempotent, and drop boundaries. MariaDB also stores a
schema comment in the same native `db.opt` file, and `ALTER DATABASE` can
rewrite only that comment while preserving the existing schema defaults.

Before this slice, MyLite's focused ownerless schema-option classifier only
accepted default charset/collation options. A writer killed after a native
schema-comment rewrite but before ownerless dictionary finish therefore lacked
positive live-peer recovery evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` accepts `COMMENT [=] TEXT_STRING` in
  `create_database_option`, and also has an `ALTER DATABASE COMMENT ...`
  current-schema form.
- `mariadb/sql/sql_db.cc` `write_db_opt()` validates `schema_comment`, loads
  existing values for omitted charset/comment fields during `SQLCOM_ALTER_DB`,
  and writes `default-character-set`, `default-collation`, and optional
  `comment=` lines to `MY_DB_OPT_FILE`.
- `mariadb/sql/sql_db.cc` `mysql_alter_db_internal()` locks the schema name and
  rewrites the schema `db.opt` file through `write_db_opt()`.
- `mariadb/sql/sql_show.cc` exposes the persisted comment through
  `INFORMATION_SCHEMA.SCHEMATA.SCHEMA_COMMENT`.

## Design

Extend MyLite's focused schema-option recovery parser to accept
`COMMENT [=] 'literal'` and `COMMENT [=] "literal"` alongside existing
`DEFAULT CHARACTER SET`, `CHARSET`, and `COLLATE` options. The parser remains
fail-closed for unquoted comment values and unrelated trailing tokens.

Add a hook-build selector, `dictionary-schema-comment-crash`, that:

- creates `ownerless_schema_comment_crash` with `latin1` defaults and an
  initial schema comment,
- creates an InnoDB table whose string column inherits `latin1`,
- keeps a live ownerless peer open,
- kills a writer after
  `ALTER DATABASE ownerless_schema_comment_crash COMMENT = 'ownerless updated schema comment'`
  completes natively but before ownerless dictionary finish,
- verifies live-peer recovery sees the updated `SCHEMA_COMMENT` and preserved
  `latin1` defaults while the native file-operation marker stays clear,
- creates a second table after recovery to prove the preserved schema defaults
  remain active, and
- verifies ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Named `ALTER DATABASE ... COMMENT = 'literal'` crash recovery at
  `dictionary-before-finish`.
- Parser support for quoted schema comments in the existing focused
  create/alter schema option grammar.
- Native `db.opt` presence, `INFORMATION_SCHEMA.SCHEMATA.SCHEMA_COMMENT`,
  inherited table collation, live-peer recovery, and reopen evidence.

Out of scope:

- Current-schema default charset/collation-only forms.
- Character-set introducers on schema comment literals.
- Invalid or overlong schema comment error cleanup.
- Exhaustive ordering of comment, charset, and collation options.
- Broader DDL/file-lifecycle recovery and long randomized external stress.

## Compatibility Impact

No user-visible SQL feature is newly invented. The slice aligns ownerless
recovery classification with MariaDB's supported schema-comment option so an
already-completed native `db.opt` rewrite can be recovered while peers remain
live.

## Directory And Lifecycle Impact

No directory layout changes. The slice exercises MariaDB's native
`datadir/<schema>/db.opt` inside the MyLite database directory, live-peer
ownerless recovery, no-live reopen, forced `.shm` rebuild, and ordinary native
exclusive reopen.

## Native Storage Impact

Schema comments are SQL-layer native metadata. The test creates InnoDB tables
before and after recovery to prove native storage remains usable and inherited
schema defaults are preserved.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. The slice adds hook test coverage and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the direct selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-comment-crash`.
- Run the focused CTest:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-schema-comment-crash$' --output-on-failure`.
- Run adjacent schema selectors.
- Run production build guard checks and format/diff checks.

## Acceptance Criteria

- The focused selector reaches the dictionary-finish fault and exits cleanly.
- Live-peer recovery observes the updated schema comment and preserved
  charset/collation while the native file-operation marker remains clear.
- The schema directory and `db.opt` stay inside the MyLite database directory.
- Tables created before and after recovery retain the expected inherited
  `latin1` metadata and row state.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- The current-schema `ALTER DATABASE COMMENT ...` form is covered by
  `docs/specs/ownerless-current-schema-comment-live-recovery/specs.md`.
- Broader schema option permutations, invalid-option cleanup, and longer
  external MariaDB/RQG stress remain planned.
