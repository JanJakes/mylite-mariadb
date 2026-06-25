# Ownerless Unique Index Idempotent Live Recovery

## Problem Statement

Ownerless unique-index idempotent crash coverage proves duplicate
`CREATE UNIQUE INDEX IF NOT EXISTS` and duplicate
`ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS` branches preserve the original
unique key after no-live recovery. These branches are metadata-only once
pre-execution index metadata proves the no-op outcome.

MyLite should recover those bounded no-op dictionary boundaries while another
ownerless peer remains live, with the native file-operation marker clear.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses top-level
  `CREATE UNIQUE INDEX opt_if_not_exists` and calls
  `Lex->add_create_index(Key::UNIQUE, ...)`.
- `mariadb/sql/sql_yacc.yy:key_def` parses table-element unique keys through
  `constraint_key_type opt_if_not_exists`, covering
  `ALTER TABLE ... ADD UNIQUE INDEX IF NOT EXISTS`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes duplicate
  `ADD KEY IF NOT EXISTS` work items by key name and emits duplicate-key-name
  diagnostics as notes instead of replacing the existing key definition.
- `packages/libmylite/src/database.cc`
  `ownerless_begin_dictionary_ddl()` chooses the ownerless dictionary recovery
  kind before native SQL execution, so a conservative
  `information_schema.statistics` lookup can distinguish duplicate no-op
  branches from real unique-index creation.

## Design

Reuse `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_INDEX_IDEMPOTENT_CREATE` for the
unique duplicate no-op branches and extend the existing bounded classifiers to
accept:

- `CREATE UNIQUE INDEX IF NOT EXISTS <index> ON <table> (...)` when
  pre-execution `information_schema.statistics` proves the index already
  exists.
- `ALTER TABLE <table> ADD UNIQUE INDEX IF NOT EXISTS <index> (...)` when
  pre-execution metadata proves the index already exists.

If metadata lookup fails, the target table is unqualified without a current
schema, or the statement does not match the bounded grammar, the classifier
returns false and the existing conservative recovery behavior applies.

Promote `dictionary-unique-index-idempotent-create-crash` and
`dictionary-alter-unique-index-idempotent-create-crash` to the held-live-peer
recovery pattern. Both selectors assert that the native file-operation marker
stays clear before and after live recovery.

## Scope

In scope:

- Duplicate top-level unique-index create no-op live recovery.
- Duplicate ALTER-table unique-index add no-op live recovery.
- Marker-clear live-peer metadata-only recovery.
- Original unique key-part preservation and duplicate-key enforcement checks.
- Attempted replacement key-part non-enforcement checks.
- Ownerless/native reopen before and after forced `.shm` rebuild.
- Standalone hook CTests for visible timing and failure attribution.

Out of scope:

- Real unique-index creation.
- Existing unique-index drop crash recovery.
- `CREATE OR REPLACE UNIQUE INDEX` replacement recovery.
- Primary-key idempotent no-op recovery.
- Multi-action ALTER statements.
- Prefix, direction, generated-column, full-text, spatial, online-option, and
  randomized DDL oracle variants.
- SQL-level table-lock fault injection and external MariaDB/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless recovery for
MariaDB-compatible idempotent unique secondary-index no-op behavior. Plain
duplicate top-level create and ALTER add still return errno 1061, and the
original unique key remains the enforced key.

## Directory And Lifecycle Impact

No directory layout changes. The no-op branches preserve native InnoDB
secondary-index metadata and do not set the ownerless native file-operation
checkpoint-needed marker. Recovery finishes the dead ownerless dictionary
generation while a peer remains live.

## Native Storage Impact

No storage format changes. MariaDB remains authoritative for the index
definition and uniqueness enforcement; MyLite only records the ownerless
dictionary boundary as metadata-only recoverable after pre-execution metadata
proves the no-op branch.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `dictionary-unique-index-idempotent-create-crash`
  - `dictionary-alter-unique-index-idempotent-create-crash`
- Run registered standalone CTests:
  - `libmylite.ownerless-dictionary-unique-index-idempotent-create-crash`
  - `libmylite.ownerless-dictionary-alter-unique-index-idempotent-create-crash`
- Run production representative selector:
  - `unique-index-ddl`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- Both focused selectors reach `dictionary-before-finish` and do not hang.
- A live peer can recover the dead dictionary owner for both no-op branches.
- The native file-operation marker remains clear before and after recovery.
- Duplicate top-level unique-index create preserves the original unique key,
  keeps plain duplicate create returning errno 1061, and keeps duplicate-key
  enforcement on the original key.
- Duplicate ALTER-table unique-index add preserves the original unique key,
  keeps plain duplicate ALTER-add returning errno 1061, and keeps duplicate-key
  enforcement on the original key.
- The attempted replacement key part remains absent and non-enforced.
- Post-recovery writes succeed.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same table rows, index metadata, and uniqueness enforcement.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-unique-index-idempotent-create-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-alter-unique-index-idempotent-create-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-(unique-index-idempotent-create|alter-unique-index-idempotent-create)-crash$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-(index-idempotent-create|alter-index-idempotent-create)-crash$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test unique-index-ddl`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- The classifier intentionally does not claim live recovery for multi-action
  ALTER statements or unique-index replacement/drop variants.
- Broader DDL/file-lifecycle recovery, primary-key idempotent no-op live
  recovery, and external MariaDB/RQG stress remain open completion work.
