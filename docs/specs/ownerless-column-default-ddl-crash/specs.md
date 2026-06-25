# Ownerless Column Default DDL Crash

## Problem

Ownerless column-default DDL coverage verifies that an already-open peer sees
`ALTER TABLE ... ALTER COLUMN ... SET DEFAULT` and
`ALTER TABLE ... ALTER COLUMN ... DROP DEFAULT` changes before subsequent DML.
The remaining crash boundary is a writer killed after MariaDB updates the
native table metadata but before MyLite publishes ownerless dictionary finish.

This slice adds hook-build recovery evidence for a representative successful
column-default metadata change.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... ALTER COLUMN col SET DEFAULT expr` and
  `ALTER TABLE ... ALTER COLUMN col DROP DEFAULT` through the alter-table
  grammar.
- `mariadb/sql/sql_table.cc` maps parser-side default changes into
  `ALTER_COLUMN_DEFAULT` handler flags before ALTER TABLE execution.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `ALTER` as ownerless
  dictionary DDL and exposes the unsafe `dictionary-before-finish` hook after
  native SQL execution but before ownerless dictionary finish.

## Design

Add an unsafe-hook selector, `dictionary-column-default-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates `app.ownerless_column_default_crash` with `value DEFAULT 10` and
  `note DEFAULT 'ready'`,
- inserts one default-backed row,
- keeps a live ownerless peer open,
- kills a writer after
  `ALTER TABLE app.ownerless_column_default_crash ALTER COLUMN value SET DEFAULT 25`
  completes natively but before ownerless dictionary finish,
- verifies metadata-only live-peer recovery with the native file-operation
  marker clear through
  `docs/specs/ownerless-metadata-alter-live-recovery/specs.md`,
- verifies the recovered `value` default is `25`,
- inserts a default-backed row that uses `25`,
- drops the `value` default after recovery,
- verifies omitting the NOT NULL `value` column fails and explicit-value inserts
  still use the unchanged `note` default, and
- verifies ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for representative
  `ALTER COLUMN ... SET DEFAULT`.
- Post-recovery `DROP DEFAULT` behavior over the recovered table.
- Recovered native `.frm` and `.ibd` files under `datadir/app/`.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustive expression-default matrices.
- Generated-column default-expression dependency rewrites.
- Randomized DDL oracle execution.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
ALTER TABLE compatibility evidence by proving a completed native column-default
metadata rewrite survives writer death at MyLite's dictionary publication
boundary.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native InnoDB table metadata,
ownerless live-peer recovery, forced `.shm` rebuild, and ordinary native
exclusive reopen.

## Native Storage Impact

Column defaults are MariaDB-native table metadata. MyLite coordinates the
ownerless dictionary boundary and verifies durable reopen behavior for the
resulting native table.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-default-crash`
- Run adjacent `column-default-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shard, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- Live-peer recovery is covered by
  `docs/specs/ownerless-metadata-alter-live-recovery/specs.md`.
- Recovery exposes `value DEFAULT 25` through `INFORMATION_SCHEMA.COLUMNS`.
- A post-recovery default-backed insert uses `25`.
- Dropping the `value` default after recovery makes omitted NOT NULL inserts
  fail while explicit-value inserts remain valid.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers one successful default rewrite, not every default expression or
  generated-column interaction.
- Broader randomized DDL oracle execution remains planned.
