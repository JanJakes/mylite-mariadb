# Ownerless Column IF EXISTS Live Recovery

## Problem Statement

Ownerless column `IF EXISTS` crash coverage proves missing
`ALTER TABLE ... MODIFY COLUMN IF EXISTS`,
`ALTER TABLE ... RENAME COLUMN IF EXISTS`,
`ALTER TABLE ... CHANGE COLUMN IF EXISTS`, and
`ALTER TABLE ... ALTER COLUMN IF EXISTS SET/DROP DEFAULT` no-op branches
preserve the original table definition after recovery. These branches are
metadata-only once pre-execution column metadata proves the no-op outcome.

MyLite should recover those bounded no-op dictionary boundaries while another
ownerless peer remains live, with the native file-operation marker clear.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `MODIFY opt_column opt_if_exists_table_element field_spec opt_place`.
- `mariadb/sql/sql_yacc.yy` parses
  `RENAME COLUMN opt_if_exists_table_element ident TO ident`.
- `mariadb/sql/sql_yacc.yy` parses
  `CHANGE opt_column opt_if_exists_table_element field_ident field_spec
  opt_place`.
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER opt_column opt_if_exists_table_element field_ident SET DEFAULT ...`
  and `ALTER opt_column opt_if_exists_table_element field_ident DROP DEFAULT`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes
  missing-column `MODIFY COLUMN IF EXISTS`, `CHANGE COLUMN IF EXISTS`, and
  `ALTER/RENAME COLUMN IF EXISTS` entries, pushing errno 1054 diagnostics as
  notes instead of mutating the table definition. It also clears
  `ALTER_CHANGE_COLUMN_DEFAULT` when no default alter-list entries remain.
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
- `ALTER TABLE <table> CHANGE [COLUMN] IF EXISTS <old_column> <new_column> ...`
  when pre-execution metadata proves the source column is absent.
- `ALTER TABLE <table> ALTER [COLUMN] IF EXISTS <column> SET DEFAULT ...`
  when pre-execution metadata proves the source column is absent.
- `ALTER TABLE <table> ALTER [COLUMN] IF EXISTS <column> DROP DEFAULT` when
  pre-execution metadata proves the source column is absent.

The modify/change/default classifiers reject comma-separated top-level ALTER
clauses so a proven missing column cannot hide a second real table mutation in
the same statement. If metadata lookup fails, the target table is unqualified
without a current schema, or the statement does not match the bounded grammar,
the classifier returns false and the existing conservative recovery behavior
applies.

Promote the missing-column `IF EXISTS` selectors to the held-live-peer recovery
pattern. Each selector asserts that the native file-operation marker stays
clear before and after live recovery.

## Scope

In scope:

- Missing `MODIFY COLUMN IF EXISTS` no-op live recovery.
- Missing `RENAME COLUMN IF EXISTS` no-op live recovery.
- Missing `CHANGE COLUMN IF EXISTS` no-op live recovery.
- Missing `ALTER COLUMN IF EXISTS SET/DROP DEFAULT` no-op live recovery.
- Missing rename/change/default no-op live recovery on generated-column/CHECK
  expression tables.
- Marker-clear live-peer metadata-only recovery.
- Original real-column metadata/default preservation.
- Missing and attempted renamed-column absence checks.
- Generated-column values and CHECK enforcement preservation.
- Ownerless/native reopen before and after forced `.shm` rebuild.
- Standalone hook CTests for visible timing and failure attribution.

Out of scope:

- Real column modify/rename recovery.
- Real column change/default recovery.
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
  - `dictionary-column-idempotent-change-crash`
  - `dictionary-column-idempotent-default-set-crash`
  - `dictionary-column-idempotent-default-drop-crash`
  - `dictionary-column-idempotent-rename-expression-crash`
  - `dictionary-column-idempotent-change-expression-crash`
  - `dictionary-column-idempotent-default-set-expression-crash`
  - `dictionary-column-idempotent-default-drop-expression-crash`
- Run registered standalone CTests:
  - `libmylite.ownerless-dictionary-column-idempotent-modify-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-rename-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-change-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-default-set-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-default-drop-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-rename-expression-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-change-expression-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-default-set-expression-crash`
  - `libmylite.ownerless-dictionary-column-idempotent-default-drop-expression-crash`
- Run production representative selector:
  - `column-idempotent-ddl`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selectors reach `dictionary-before-finish` and do not hang.
- A live peer can recover the dead dictionary owner for each no-op branch.
- The native file-operation marker remains clear before and after recovery.
- Missing `MODIFY COLUMN IF EXISTS` recovery preserves original real-column
  metadata/default, keeps the missing column absent, and keeps plain missing
  modify returning errno 1054.
- Missing `RENAME COLUMN IF EXISTS` recovery preserves the real column, keeps
  both the missing and attempted renamed columns absent, and keeps plain
  missing rename returning errno 1054.
- Missing `CHANGE COLUMN IF EXISTS` recovery preserves original real-column
  metadata/default, keeps the missing and attempted changed columns absent, and
  keeps plain missing change returning errno 1054.
- Missing `ALTER COLUMN IF EXISTS SET/DROP DEFAULT` recovery preserves the
  original real-column default, keeps the missing column absent, and keeps plain
  missing default changes returning errno 1054.
- Expression-table variants preserve generated-column values, real-column
  defaults, CHECK enforcement, and missing attempted names.
- Post-recovery writes succeed.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same table metadata and rows.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test -j2`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-primitives$|^libmylite\.ownerless-dictionary-column-idempotent-(modify|rename|rename-expression|change-expression|default-set-expression|default-drop-expression|change|default-set|default-drop)-crash$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test column-idempotent-ddl`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

Stress note:

- The final `ownerless-cross-process-ddl-stress` rerun passed in 58.19
  seconds.

Formatting note:

- The first `format-check` run flagged only the new helper's line wrapping in
  `database.cc`; after `clang-format`, `format-check` passed.

## Risks And Follow-Up

- The classifier intentionally does not claim live recovery for multi-clause
  ALTER statements or broader missing-column `IF EXISTS` variants.
- Broader DDL/file-lifecycle recovery and external MariaDB/RQG stress remain
  open completion work.
