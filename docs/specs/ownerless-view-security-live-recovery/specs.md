# Ownerless View Security Live Recovery

## Problem Statement

Ownerless view security crash coverage proves deterministic definer create,
invoker replacement, and definer-security alter recovery after a writer dies at
the dictionary publication boundary. That coverage still waited until every
peer had exited even though the relevant MariaDB durable state is metadata-only
view definition state, and the ownerless view recovery lane can classify the
completed statement without native file-operation marker evidence.

This slice promotes the focused security/definer view crash selectors to
metadata-only live recovery while another ownerless peer remains open.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - The view grammar accepts `DEFINER` and `SQL SECURITY` clauses for
    `CREATE VIEW`, `CREATE OR REPLACE VIEW`, and `ALTER VIEW`.
- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_VIEW` dispatches create, replace, and alter view execution
    through `mysql_create_view()`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` processes the parsed definer and security metadata
    before registration.
  - `sp_process_definer()` validates and normalizes explicit definer metadata.
  - `mysql_register_view()` persists `view->definer` and `view->view_suid` in
    the native view definition file.
- `packages/libmylite/src/database.cc`
  - View DDL recovery kinds are metadata-only and do not require native
    file-operation checkpoint marker evidence.
  - The focused view DDL recovery classifiers cover completed native
    definition rewrites after the required `AS` token.
  - The focused view classifier now consumes `DEFINER=CURRENT_USER` and
    `SQL SECURITY DEFINER|INVOKER` clauses before `VIEW` for create, replace,
    and alter view recovery.

## Design

Reuse the existing metadata-only view live-recovery lane for focused
security/definer view DDL:

- `CREATE DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW ...`
- `CREATE OR REPLACE SQL SECURITY INVOKER VIEW ...`
- `ALTER DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW ...`

Each selector keeps a live ownerless peer open after killing the writer, opens a
new ownerless handle, recovers the completed native view metadata, verifies the
expected `SECURITY_TYPE`, non-empty `DEFINER`, `.frm` presence, query behavior,
and base-table writes, then confirms the native file-operation marker remains
clear. After the held peer exits, the existing ownerless/native reopen and
forced `.shm` rebuild checks continue to prove final state.

## Scope

In scope:

- Focused explicit-definer `CREATE VIEW` live recovery.
- Focused `CREATE OR REPLACE VIEW` replacement to `SQL SECURITY INVOKER` live
  recovery.
- Focused `ALTER DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW` live recovery.
- Recovered `.frm` presence, `INFORMATION_SCHEMA.VIEWS.SECURITY_TYPE`, and
  non-empty `DEFINER` metadata.
- Query behavior and base-table writes while another ownerless peer remains
  live.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Invalid or missing definers.
- MariaDB account lifecycle and privilege enforcement.
- Stored functions, routines, packages, and randomized view oracles.
- SQL-level table-lock fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB security/definer view metadata is visible
to a new ownerless opener before every peer exits.

Privilege and account semantics remain outside this bounded claim.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The live-recovery path consumes the
recoverable dictionary prefinish marker and updates ownerless dictionary state
without requiring native file-operation marker evidence. Final no-live close,
forced `.shm` rebuild, and native exclusive reopen continue to verify the
recovered native `.frm` state.

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
  - `dictionary-view-security-create-crash`
  - `dictionary-view-security-replace-crash`
  - `dictionary-view-security-alter-crash`
- Run focused CTest entries for all three selectors.
- Run adjacent metadata-only view live-recovery selectors.
- Run production embedded ownerless view security, view DDL, and broader DDL
  selectors.
- Run targeted ownerless DDL stress.
- Run production-build guards, `format-check`, and `git diff --check`.

## Verification Results

Collected on branch `ownerless-concurrency` after the implementation:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-security-create-crash` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-security-replace-crash` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-security-alter-crash` passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-dictionary-view-security-(create|replace|alter)-crash$' --output-on-failure` passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-dictionary-view-(create|drop|replace|alter|column-list-(create|replace|alter)|check-option-(create|replace|alter)|nested-check-option-(outer-replace|inner-alter)|security-(create|replace|alter))-crash$' --output-on-failure` passed.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test view-security-definer` passed.
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

- A killed security/definer view writer is recovered by a new ownerless opener
  while another ownerless peer remains live.
- The native file-operation checkpoint-needed marker stays clear for all three
  metadata-only view forms.
- Create recovery exposes `SECURITY_TYPE='DEFINER'`, non-empty `DEFINER`
  metadata, the `.frm` file, and queryable view rows.
- Replacement recovery exposes `SECURITY_TYPE='INVOKER'`, non-empty `DEFINER`
  metadata, the replacement predicate, and queryable view rows.
- Alter recovery exposes `SECURITY_TYPE='DEFINER'`, non-empty `DEFINER`
  metadata, the altered predicate, and queryable view rows.
- Ownerless and ordinary native reopen observe the same recovered state before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic security/definer metadata recovery, not full
  privilege enforcement.
- Invalid-definer, account-lifecycle, stored-function view, and randomized view
  oracle coverage remain planned.
- Idempotent/no-op and invalid-dependency view drop recovery are covered by
  separate live-recovery slices.
- Trigger, schema, and broader metadata-only DDL live recovery remain planned.
- Longer external MariaDB/RQG-style stress remains planned after bounded
  recovery classes stop producing correctness fixes.
