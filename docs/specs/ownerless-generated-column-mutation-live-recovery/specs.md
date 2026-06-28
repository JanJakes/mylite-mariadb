# Ownerless Generated Column Mutation Live Recovery

## Problem Statement

Ownerless column ALTER live-recovery coverage already proves ordinary
`DROP COLUMN`, `MODIFY COLUMN`, `CHANGE COLUMN`, and `RENAME COLUMN` crash
boundaries, plus generated-column/CHECK dependent-expression preservation for
missing-column no-op paths. The compatibility matrix still called out real
generated-column `DROP`, `MODIFY`, and `CHANGE` live recovery as planned.

This slice closes that bounded gap for representative stored and virtual
generated columns. A writer is killed after MariaDB accepts the native ALTER and
before MyLite publishes ownerless dictionary finish; another ownerless peer
stays live until recovery proves the native file-operation marker remains
retained and later drains.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses generated-column definitions through
  `field_def`, attaching `Virtual_column_info` to the last field, and
  distinguishes virtual from stored generated columns in
  `vcol_opt_specifier`.
- `mariadb/sql/field.cc` validates generated-column expressions in
  `Column_definition::check()` through `check_expression()`, including stored
  versus virtual filtering.
- `mariadb/sql/table.cc` `parse_vcol_defs()` reloads generated-column
  expression metadata from `.frm` `TABLE_SHARE::vcol_defs`; recovery must
  preserve those definitions, not just column names.
- `mariadb/storage/innobase/handler/handler0alter.cc` tracks virtual-column
  add/drop state in `ha_innobase_inplace_ctx` and collects dropped virtual
  columns in `prepare_inplace_drop_virtual()`. Stored generated columns are
  stored in the clustered record and exercise the ordinary table-definition
  mutation/rebuild path.

## Design

Add three unsafe hook selectors to
`mylite_ownerless_cross_process_sql_test`:

- `dictionary-generated-column-drop-crash`
- `dictionary-generated-column-modify-crash`
- `dictionary-generated-column-change-crash`

Extend the ownerless dictionary recovery classifier so these bounded generated
column mutations reuse the existing native-file-operation recovery kinds:

- `DROP COLUMN` is recoverable only when each dropped column is a generated
  column in `information_schema.columns`.
- `MODIFY COLUMN` and `CHANGE COLUMN` are recoverable only for generated-column
  definitions that retain MariaDB-generated storage semantics and avoid known
  rejected generated-expression functions.
- Failed generated-column DDL shapes that MariaDB rejects before durable
  metadata becomes safe, including nondeterministic generated expressions and
  generated-column primary keys, remain non-live-only recovery instead of being
  converted into a recoverable no-op marker.

Each selector:

1. Creates a small InnoDB table with one stored and one virtual generated
   column.
2. Kills an ownerless writer at `dictionary-before-finish` after a successful
   generated-column mutation ALTER.
3. Opens a new ownerless writer while a peer remains live.
4. Verifies recovered column metadata and generated values.
5. Performs post-recovery DML through the recovered table definition.
6. Verifies native file-operation marker retention while the peer is live, final
   marker drain after peer release, ordinary native reopen, ownerless reopen,
   and forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- representative stored and virtual generated-column `DROP COLUMN`,
  `MODIFY COLUMN`, and `CHANGE COLUMN` live recovery,
- dictionary-before-finish crash injection,
- recovered computed-value behavior and old/new column-name visibility,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- exhaustive generated-column expression forms,
- generated-column foreign-key action crash matrices beyond existing coverage,
- SQL-level table-lock fault injection,
- multi-clause ALTER combinations beyond the paired stored/virtual generated
  column mutations,
- randomized external DDL oracle execution.

## Compatibility Impact

No new SQL syntax or behavior is enabled. The slice strengthens evidence for
MariaDB-compatible generated-column ALTER recovery in ownerless mode and keeps
known failed generated-column DDL on the existing conservative recovery path.

## Directory And Lifecycle Impact

No directory layout changes. The selectors exercise native `.frm` expression
metadata and InnoDB tablespace state inside the MyLite-owned database
directory. The native file-operation checkpoint-needed marker remains the
conservative live-peer policy for these generated-column table-definition
mutations.

## Native Storage Impact

Stored generated columns mutate the stored InnoDB row definition; virtual
generated columns exercise InnoDB virtual-column alter bookkeeping. The slice
does not change page-version WAL format, redo/checkpoint policy, or file
format.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused CTests:
  - `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-generated-column-(drop|modify|change)-crash$' --output-on-failure`
- Run adjacent generated-column hook crash selectors.
- Run the existing failed generated-column hook crash selector to prove
  rejected generated expressions and generated-column primary keys still leave
  clean metadata only after no-live recovery.
- Run focused production generated-column SQL selector.
- Run production-build audit, format checks, and `git diff --check`.

## Acceptance Criteria

- Each focused selector reaches the dictionary fault hook and exits without
  hanging.
- Live recovery preserves generated-column metadata and computed values.
- Failed generated-column `CREATE TABLE`/`ALTER TABLE`/primary-key DDL remains
  excluded from live recoverable markers.
- Post-recovery DML succeeds while another ownerless peer is still live.
- The native file-operation marker remains set while the peer is live and
  drains after no-live recovery.
- Ownerless reopen, ordinary native reopen, and forced `.shm` rebuild observe
  identical final state.

## Risks And Unresolved Questions

- The selectors cover representative stored and virtual generated-column
  mutation classes, not every generated expression or online-option spelling.
- Multi-clause ALTER mixtures with generated columns and unrelated table changes
  remain planned.
- Full external MariaDB/RQG long-running DDL stress remains planned.
