# Ownerless Column Idempotent Live Recovery

## Problem Statement

Ownerless column-idempotent DDL crash coverage proves that duplicate
`ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing
`ALTER TABLE ... DROP COLUMN IF EXISTS` no-op branches preserve the original
table definition after no-live recovery. These branches are metadata-only once
pre-execution column metadata proves the no-op outcome.

MyLite should recover those bounded no-op dictionary boundaries while another
ownerless peer remains live, with the native file-operation marker clear.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ADD opt_column opt_if_not_exists_table_element` and
  `DROP opt_column opt_if_exists_table_element field_ident`.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes duplicate
  `ADD COLUMN IF NOT EXISTS` entries when the column already exists and
  missing `DROP COLUMN IF EXISTS` entries when the column is absent, emitting
  note diagnostics instead of changing the table definition.
- `packages/libmylite/src/database.cc`
  `ownerless_begin_dictionary_ddl()` chooses the ownerless dictionary recovery
  kind before native SQL execution, so a conservative `information_schema`
  column lookup can distinguish no-op branches from real add/drop branches.

## Design

Add metadata-only recovery kinds:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_COLUMN_IDEMPOTENT_ADD`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_COLUMN_IDEMPOTENT_DROP`

Classify only bounded single-clause ALTER statements:

- `ALTER TABLE <table> ADD [COLUMN] IF NOT EXISTS <column> ...` when
  pre-execution `information_schema.columns` proves the column already exists.
- `ALTER TABLE <table> DROP [COLUMN] IF EXISTS <column>` when pre-execution
  metadata proves the column is absent.

The add classifier rejects comma-separated ALTER clauses so a proven duplicate
column add cannot hide a second real table mutation in the same statement. If
metadata lookup fails, the target table is unqualified without a current
schema, or the statement does not match the bounded grammar, the classifier
returns false and the existing recovery behavior applies.

Promote `dictionary-column-idempotent-add-crash` and
`dictionary-column-idempotent-drop-crash` to the held-live-peer recovery
pattern. Both selectors assert that the native file-operation marker stays
clear before and after live recovery.

## Scope

In scope:

- Duplicate `ADD COLUMN IF NOT EXISTS` no-op recovery for an existing column.
- Missing `DROP COLUMN IF EXISTS` no-op recovery for an absent column.
- Marker-clear live-peer metadata-only recovery.
- Original column/default preservation and missing-column absence checks.
- Ownerless/native reopen before and after forced `.shm` rebuild.
- Standalone hook CTests for visible timing and failure attribution.

Out of scope:

- Real column add/drop recovery.
- Multi-clause ALTER statements.
- `MODIFY COLUMN IF EXISTS`, `RENAME COLUMN IF EXISTS`,
  `CHANGE COLUMN IF EXISTS`, and default-alter `IF EXISTS` no-ops.
- Partitioned, period, generated-column expression, index, foreign-key, and
  CHECK table-element variants beyond the existing focused tests.
- SQL-level table-lock fault injection for native table-wait paths.
- External randomized DDL/RQG oracle execution.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless recovery for
MariaDB-compatible idempotent column no-op behavior. Plain duplicate add still
returns errno 1060, and plain missing drop still returns errno 1091.

## Directory And Lifecycle Impact

No directory layout changes. The no-op branches preserve the native InnoDB
table definition and do not set the ownerless native file-operation
checkpoint-needed marker. Recovery finishes the dead ownerless dictionary
generation while a peer remains live.

## Native Storage Impact

No storage format changes. MariaDB remains authoritative for the table
definition; MyLite only records the ownerless dictionary boundary as
metadata-only recoverable after pre-execution metadata proves the no-op branch.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_ownerless_primitives_test` with `ownerless-test-hooks`.
- Run primitive recovery-kind coverage.
- Run focused selectors:
  - `dictionary-column-idempotent-add-crash`
  - `dictionary-column-idempotent-drop-crash`
- Run registered standalone CTests:
  - `libmylite.ownerless-dictionary-column-idempotent-add-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-drop-crash`
- Run production representative selector:
  - `column-idempotent-ddl`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- Both focused selectors reach `dictionary-before-finish` and do not hang.
- A live peer can recover the dead dictionary owner for both no-op branches.
- The native file-operation marker remains clear before and after recovery.
- Duplicate idempotent add preserves the original column/default and keeps
  plain duplicate add returning errno 1060.
- Missing idempotent drop preserves the real column, keeps the missing column
  absent, and keeps plain missing drop returning errno 1091.
- Post-recovery writes succeed.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same table metadata and rows.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test -j2`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-idempotent-add-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-idempotent-drop-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-column-idempotent-(add|drop)-crash$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test column-idempotent-ddl`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- The classifier intentionally does not claim live recovery for multi-clause
  ALTER statements or broader column `IF EXISTS` variants.
- Broader DDL/file-lifecycle recovery and external MariaDB/RQG stress remain
  open completion work.
