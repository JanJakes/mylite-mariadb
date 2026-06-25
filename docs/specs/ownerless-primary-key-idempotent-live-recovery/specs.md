# Ownerless Primary-Key Idempotent Live Recovery

## Problem Statement

Ownerless primary-key idempotent crash coverage proves duplicate
`ALTER TABLE ... ADD PRIMARY KEY IF NOT EXISTS` preserves the original
clustered primary key after no-live recovery. This branch is metadata-only once
pre-execution primary-key metadata proves the no-op outcome.

MyLite should recover that bounded no-op dictionary boundary while another
ownerless peer remains live, with the native file-operation marker clear.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:key_def` parses
  `constraint_key_type opt_if_not_exists` through `Lex->add_key()`.
- `mariadb/sql/sql_yacc.yy:constraint_key_type` maps `PRIMARY KEY` to
  `Key::PRIMARY`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes duplicate
  `ADD PRIMARY KEY IF NOT EXISTS` work when a primary key already exists,
  preserving the current primary-key definition and returning success with
  note diagnostics.
- `packages/libmylite/src/database.cc`
  `ownerless_begin_dictionary_ddl()` chooses the ownerless dictionary recovery
  kind before native SQL execution, so a conservative
  `information_schema.statistics` lookup can distinguish the duplicate no-op
  branch from real primary-key creation.

## Design

Reuse `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_INDEX_IDEMPOTENT_CREATE` for the
duplicate primary-key no-op branch and add a bounded classifier for:

- `ALTER TABLE <table> ADD PRIMARY KEY IF NOT EXISTS (...)` when
  pre-execution `information_schema.statistics` proves index `PRIMARY` already
  exists on the table.

If metadata lookup fails, the target table is unqualified without a current
schema, or the statement does not match the bounded grammar, the classifier
returns false and the existing conservative recovery behavior applies.

Promote `dictionary-primary-key-idempotent-crash` to the held-live-peer
recovery pattern. The selector asserts that the native file-operation marker
stays clear before and after live recovery.

## Scope

In scope:

- Duplicate `ADD PRIMARY KEY IF NOT EXISTS` no-op live recovery.
- Marker-clear live-peer metadata-only recovery.
- Original `PRIMARY(id)` preservation and duplicate-key enforcement checks.
- Attempted `PRIMARY(code)` absence and non-uniqueness checks.
- Ownerless/native reopen before and after forced `.shm` rebuild.
- Standalone hook CTest for visible timing and failure attribution.

Out of scope:

- Real primary-key creation or replacement.
- `DROP PRIMARY KEY IF EXISTS`, which is not a MariaDB grammar branch for this
  base.
- Composite, descending, AUTO_INCREMENT, generated-column, foreign-key, and
  algorithm/lock-option primary-key variants.
- Multi-action ALTER statements.
- SQL-level table-lock fault injection and external MariaDB/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless recovery for
MariaDB-compatible idempotent primary-key no-op behavior. Plain duplicate
primary-key add still returns errno 1068, the original primary key remains
enforced, and the attempted candidate key remains non-unique.

## Directory And Lifecycle Impact

No directory layout changes. The no-op branch preserves native InnoDB
primary-key metadata and does not set the ownerless native file-operation
checkpoint-needed marker. Recovery finishes the dead ownerless dictionary
generation while a peer remains live.

## Native Storage Impact

No storage format changes. MariaDB remains authoritative for clustered
primary-key metadata and enforcement; MyLite only records the ownerless
dictionary boundary as metadata-only recoverable after pre-execution metadata
proves the no-op branch.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selector:
  - `dictionary-primary-key-idempotent-crash`
- Run registered standalone CTest:
  - `libmylite.ownerless-dictionary-primary-key-idempotent-crash`
- Run production representative selector:
  - `primary-key-ddl`
- Run ownerless DDL stress.
- Run production-build guards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selector reaches `dictionary-before-finish` and does not hang.
- A live peer can recover the dead dictionary owner for the no-op branch.
- The native file-operation marker remains clear before and after recovery.
- Duplicate idempotent ADD-primary recovery keeps `PRIMARY(id)`, leaves
  `PRIMARY(code)` absent, and keeps plain duplicate primary-key add returning
  errno 1068.
- Duplicate `id` inserts fail, while duplicate `code` inserts with a new `id`
  succeed.
- Post-recovery writes succeed.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same table rows, primary-key metadata, and uniqueness enforcement.

## Verification Results

Passed:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-primary-key-idempotent-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-primary-key-idempotent-crash$' --output-on-failure`
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test primary-key-ddl`
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
- `cmake --build --preset prod --target format-check`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `git diff --check`

## Risks And Follow-Up

- The classifier intentionally does not claim live recovery for multi-action
  ALTER statements or primary-key replacement/drop variants.
- Broader DDL/file-lifecycle recovery and external MariaDB/RQG stress remain
  open completion work.
