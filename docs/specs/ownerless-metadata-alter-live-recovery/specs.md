# Ownerless Metadata ALTER Live Recovery

## Problem Statement

Ownerless hook coverage already proves representative `ALTER TABLE ... COMMENT`
and `ALTER TABLE ... ALTER COLUMN ... SET DEFAULT` crash recovery after MariaDB
finishes native metadata update but before MyLite publishes ownerless dictionary
finish. Those selectors still used no-live recovery: an unrelated live
ownerless peer kept cleanup busy until the peer exited.

These focused ALTER forms update table-definition metadata without proving a
native InnoDB file-operation checkpoint requirement. MyLite should finish the
ownerless dictionary boundary while another ownerless peer remains live when
the completed statement is one of the covered metadata-only forms.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses table-option `COMMENT` with optional `=`
  and records `HA_CREATE_USED_COMMENT` in `HA_CREATE_INFO`.
- `mariadb/sql/sql_yacc.yy` parses `ALTER opt_column ... SET DEFAULT
  column_default_expr` and `DROP DEFAULT` through the ALTER TABLE grammar.
- `mariadb/sql/sql_table.cc` maps parser-side column default changes into
  `ALTER_COLUMN_DEFAULT` handler flags before ALTER execution.
- `mariadb/sql/handler.cc:handler::check_if_supported_inplace_alter()` lists
  `ALTER_COLUMN_DEFAULT` and `ALTER_CHANGE_CREATE_OPTION` in the ordinary
  in-place metadata operation set for native handlers.

## Design

- Add metadata-only dictionary recovery kinds for:
  - representative table-comment ALTER,
  - representative column `SET DEFAULT` ALTER.
- Add conservative classifiers for:
  - `ALTER TABLE <table> COMMENT [=] <literal>`,
  - `ALTER TABLE <table> ALTER [COLUMN] <column> SET DEFAULT <single-token expr>`.
- Keep `DROP DEFAULT`, generated-column default dependencies, multi-action
  ALTER, broader table options, expression-default matrices, and table rebuild
  variants outside this slice.
- Promote the existing table-comment and column-default crash selectors to the
  held-live-peer recovery path and assert the native file-operation checkpoint
  marker remains clear before follow-up DML or DDL.
- Register both selectors as standalone hook CTests for visible timing and
  failure attribution.

## Affected Layers

- Ownerless dictionary-recovery classification and recovery-kind state.
- Ownerless cross-process SQL hook coverage and CTest registration.
- Compatibility and ownerless concurrency evidence docs.

## Compatibility Impact

No SQL behavior is newly enabled. The slice narrows an ownerless recovery gap
for MariaDB-compatible metadata ALTER statements that have already completed
native execution.

## DDL Metadata Routing Impact

MariaDB continues to route table comments and column defaults through its
native ALTER TABLE metadata path. MyLite only records the ownerless recovery
kind before statement execution and consumes the ownerless dictionary boundary
after a dead writer reaches the tested post-native-success hook.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The slice uses existing native
table metadata under the MyLite database directory, existing ownerless
dictionary generation state, and the existing ownerless/native reopen
lifecycle.

## Native Storage Impact

The promoted paths are treated as metadata-only only for the simple covered
forms. Any shape that may require broader native file lifecycle proof remains
on the existing conservative recovery path.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-column-default-crash`
  - `dictionary-table-comment-crash`
- Run the registered hook CTest subset for those selectors.
- Run adjacent production embedded `column-default-ddl` and
  `table-comment-ddl`.
- Run ownerless DDL stress because dictionary recovery classification changed.
- Run ownerless primitive recovery-kind coverage, `format-check`, CI
  production-build guards, and `git diff --check`.

## Acceptance Criteria

- Column-default and table-comment recovery completes while another ownerless
  peer remains live.
- The native file-operation checkpoint marker remains clear for both live
  recovery paths.
- Recovered column default and table comment metadata are visible through
  `INFORMATION_SCHEMA`.
- Follow-up DML and the existing post-recovery `DROP DEFAULT` check still
  succeed.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same rows and metadata.

## Risks And Unresolved Questions

- The classifier intentionally covers narrower syntax than MariaDB's full ALTER
  TABLE grammar.
- `DROP DEFAULT`, expression-default variants, generated-column dependencies,
  broader table options, and multi-action ALTER still need separate proof
  before live recovery can be claimed.
