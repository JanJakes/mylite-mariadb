# Ownerless View Idempotent Live Recovery

## Problem Statement

Ownerless view-idempotent crash coverage proves duplicate
`CREATE VIEW IF NOT EXISTS` preserves the original view definition and missing
`DROP VIEW IF EXISTS` preserves real view state after a writer dies at the
dictionary publication boundary. That coverage still waited for no-live
recovery, even though both covered forms are metadata-only no-op boundaries and
do not require native file-operation marker evidence.

This slice promotes the focused idempotent/no-op view crash selectors to
metadata-only live recovery while another ownerless peer remains open.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `CREATE VIEW` accepts `opt_if_not_exists` before `table_ident`.
  - `DROP VIEW` accepts `opt_if_exists`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` treats an existing view under `IF NOT EXISTS` as a
    successful no-op with diagnostics and preserves the current definition.
- `mariadb/sql/sql_table.cc`
  - `mysql_rm_table_no_locks()` treats missing objects under `IF EXISTS` as
    successful no-op drops with diagnostics.
- `packages/libmylite/src/database.cc`
  - View DDL recovery kinds are metadata-only and do not require native
    file-operation checkpoint marker evidence.
  - The focused create-view recovery classifier now consumes optional
    `IF NOT EXISTS` after `VIEW`.
  - The focused drop-view recovery classifier now consumes optional `IF EXISTS`
    after `DROP VIEW`.

## Design

Reuse the existing metadata-only create/drop view recovery kinds for focused
idempotent no-op view DDL:

- duplicate `CREATE VIEW IF NOT EXISTS <view> AS ...`
- missing `DROP VIEW IF EXISTS <view>`

Each selector keeps a live ownerless peer open after killing the writer, opens a
new ownerless handle, recovers the completed no-op metadata boundary, verifies
original-view preservation or missing-view absence, confirms post-recovery DML
through the real view, and keeps the native file-operation marker clear. After
the held peer exits, the existing ownerless/native reopen and forced `.shm`
rebuild checks continue to prove final state.

## Scope

In scope:

- Duplicate `CREATE VIEW IF NOT EXISTS` live recovery preserving the original
  view definition and duplicate plain-create errno.
- Missing `DROP VIEW IF EXISTS` live recovery preserving the real view and
  keeping missing-view metadata absent.
- Native `.frm` presence/absence checks for real and missing views.
- Query behavior and base-table writes while another ownerless peer remains
  live.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE VIEW`, `ALTER VIEW`, explicit column lists, check-option,
  nested check-option, and security/definer view recovery, which are covered by
  separate live-recovery slices.
- Invalid dependency view live recovery.
- Trigger, schema, and broader metadata-only DDL live recovery.
- SQL-level table-lock fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB idempotent no-op view DDL is visible to a
new ownerless opener before every peer exits.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The live-recovery path consumes the
recoverable dictionary prefinish marker and updates ownerless dictionary state
without requiring native file-operation marker evidence. Final no-live close,
forced `.shm` rebuild, and native exclusive reopen continue to verify the
recovered native view metadata state.

## Native Storage Impact

The base tables are InnoDB, but the promoted recovery class is metadata-only
view definition recovery. The slice does not change InnoDB page-version WAL,
redo/checkpoint policy, page flushing, or native tablespace file lifecycle.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-view-idempotent-create-crash`
  - `dictionary-view-idempotent-drop-crash`
- Run focused CTest entries for both selectors.
- Run adjacent metadata-only view live-recovery selectors.
- Run production embedded ownerless view idempotent and broader view/DDL
  selectors.
- Run targeted ownerless DDL stress.
- Run production-build guards, `format-check`, and `git diff --check`.

## Verification Results

Collected on branch `ownerless-concurrency` after the implementation:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-idempotent-create-crash` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-idempotent-drop-crash` passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-view-idempotent-(create|drop)-crash$' --output-on-failure` passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-dictionary-view-(create|drop|replace|alter|column-list-(create|replace|alter)|check-option-(create|replace|alter)|nested-check-option-(outer-replace|inner-alter)|security-(create|replace|alter)|idempotent-(create|drop))-crash$' --output-on-failure` passed.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test view-idempotent-ddl` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test view-ddl` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test ddl-broader` passed.
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure` passed.
- `cmake --build --preset prod --target format-check` passed.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure` passed.
- `git diff --check` passed.
- Cleanup scans found no `/tmp/mylite-ownerless-*` directories and no ownerless test processes.

## Acceptance Criteria

- A killed idempotent/no-op view writer is recovered by a new ownerless opener
  while another ownerless peer remains live.
- The native file-operation checkpoint-needed marker stays clear for both
  metadata-only no-op view forms.
- Duplicate idempotent create recovery keeps the original view definition,
  leaves the attempted replacement definition absent, and keeps plain duplicate
  create returning errno 1050.
- Missing idempotent drop recovery keeps the real view present and queryable
  while the missing view remains absent.
- Ownerless and ordinary native reopen observe the same recovered state before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic no-op view DDL recovery, not every idempotent view
  lifecycle spelling.
- Invalid-dependency view drop recovery is covered separately by
  `ownerless-view-invalid-dependency-drop-live-recovery`.
- Trigger, schema, and broader metadata-only DDL live recovery remain planned.
- Longer external MariaDB/RQG-style stress remains planned after bounded
  recovery classes stop producing correctness fixes.
