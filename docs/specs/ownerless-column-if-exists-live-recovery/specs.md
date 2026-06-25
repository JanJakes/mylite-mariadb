# Ownerless Column IF EXISTS Live Recovery

## Problem Statement

Ownerless column `IF EXISTS` crash coverage proves missing
`ALTER TABLE ... MODIFY COLUMN IF EXISTS` and
`ALTER TABLE ... RENAME COLUMN IF EXISTS` no-op branches preserve the original
table definition after no-live recovery. These branches are metadata-only once
pre-execution column metadata proves the no-op outcome.

MyLite should recover those bounded no-op dictionary boundaries while another
ownerless peer remains live, with the native file-operation marker clear.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `MODIFY opt_column opt_if_exists_table_element field_spec opt_place`.
- `mariadb/sql/sql_yacc.yy` parses
  `RENAME COLUMN opt_if_exists_table_element ident TO ident`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes
  missing-column `MODIFY COLUMN IF EXISTS` and `ALTER/RENAME COLUMN IF EXISTS`
  entries, pushing errno 1054 diagnostics as notes instead of mutating the
  table definition.
- `packages/libmylite/src/database.cc`
  `ownerless_begin_dictionary_ddl()` chooses the ownerless dictionary recovery
  kind before native SQL execution, so a conservative
  `information_schema.columns` lookup can distinguish missing-column no-op
  branches from real column mutations.

## Design

Add `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_COLUMN_IF_EXISTS_NOOP` as a
metadata-only ownerless dictionary recovery kind for proven missing-column
`IF EXISTS` no-op branches.

Classify only bounded single-clause ALTER statements:

- `ALTER TABLE <table> MODIFY [COLUMN] IF EXISTS <column> ...` when
  pre-execution `information_schema.columns` proves the column is absent.
- `ALTER TABLE <table> RENAME COLUMN IF EXISTS <column> TO <new_column>` when
  pre-execution metadata proves the source column is absent.

The modify classifier rejects comma-separated ALTER clauses so a proven missing
column cannot hide a second real table mutation in the same statement. If
metadata lookup fails, the target table is unqualified without a current
schema, or the statement does not match the bounded grammar, the classifier
returns false and the existing conservative recovery behavior applies.

Promote `dictionary-column-idempotent-modify-crash` and
`dictionary-column-idempotent-rename-crash` to the held-live-peer recovery
pattern. Both selectors assert that the native file-operation marker stays
clear before and after live recovery.

## Scope

In scope:

- Missing `MODIFY COLUMN IF EXISTS` no-op live recovery.
- Missing `RENAME COLUMN IF EXISTS` no-op live recovery.
- Marker-clear live-peer metadata-only recovery.
- Original real-column metadata/default preservation.
- Missing and attempted renamed-column absence checks.
- Ownerless/native reopen before and after forced `.shm` rebuild.
- Standalone hook CTests for visible timing and failure attribution.

Out of scope:

- Real column modify/rename recovery.
- Missing `CHANGE COLUMN IF EXISTS` and `ALTER COLUMN IF EXISTS SET/DROP
  DEFAULT` no-op live recovery.
- Generated-column/CHECK expression-table variants.
- Multi-clause ALTER statements.
- Period, partition, index, and foreign-key variants.
- SQL-level table-lock fault injection and external MariaDB/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless recovery for
MariaDB-compatible missing-column `IF EXISTS` no-op behavior. Plain missing
modify/rename retries still return errno 1054, and the original real column
definition remains unchanged.

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
  - `dictionary-column-idempotent-modify-crash`
  - `dictionary-column-idempotent-rename-crash`
- Run registered standalone CTests:
  - `libmylite.ownerless-dictionary-column-idempotent-modify-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-rename-crash`
- Run production representative selector:
  - `column-idempotent-ddl`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- Both focused selectors reach `dictionary-before-finish` and do not hang.
- A live peer can recover the dead dictionary owner for both no-op branches.
- The native file-operation marker remains clear before and after recovery.
- Missing `MODIFY COLUMN IF EXISTS` recovery preserves original real-column
  metadata/default, keeps the missing column absent, and keeps plain missing
  modify returning errno 1054.
- Missing `RENAME COLUMN IF EXISTS` recovery preserves the real column, keeps
  both the missing and attempted renamed columns absent, and keeps plain
  missing rename returning errno 1054.
- Post-recovery writes succeed.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same table metadata and rows.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test -j2`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$' --output-on-failure`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-idempotent-modify-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-column-idempotent-rename-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-column-idempotent-(modify|rename)-crash$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test column-idempotent-ddl`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

Stress note:

- The first `ownerless-cross-process-ddl-stress` run exhausted statement-lock
  retries on an unrelated `ALTER TABLE ... ADD COLUMN` stress phase after
  182.73 seconds. After removing the generated temp directory and rerunning the
  same CTest, it passed in 58.32 seconds.

## Risks And Follow-Up

- The classifier intentionally does not claim live recovery for multi-clause
  ALTER statements or broader missing-column `IF EXISTS` variants.
- Missing `CHANGE COLUMN IF EXISTS`, default-alter `IF EXISTS`, and expression
  table variants remain open follow-up slices.
- Broader DDL/file-lifecycle recovery and external MariaDB/RQG stress remain
  open completion work.
