# Ownerless Nested View Check-Option Live Recovery

## Problem Statement

Ownerless check-option view live recovery covers focused single-view create,
replacement, and alter forms. Nested check-option crash coverage still waited
for no-live recovery even though MariaDB had already rewritten the native view
definition and the focused view recovery classifiers can identify the completed
statement.

This slice promotes focused nested check-option outer replacement and inner
alteration crash selectors to metadata-only live recovery while another
ownerless peer remains open.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `CREATE OR REPLACE VIEW` and `ALTER VIEW` parse `view_select`, including
    `view_check_option`.
- `mariadb/sql/sql_view.cc`
  - `mysql_make_view()` records effective check-option propagation through
    `LEX::get_effective_with_check()`.
  - `mysql_register_view()` persists the rewritten native view definition.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::prep_check_option()` propagates cascaded checks into
    underlying views but does not propagate local checks.
  - `TABLE_LIST::view_check_option()` raises `ER_VIEW_CHECK_FAILED` for inner
    or outer predicate violations.
- `packages/libmylite/src/database.cc`
  - View DDL recovery kinds are metadata-only and do not require native
    file-operation checkpoint marker evidence.
  - Focused view classifiers accept the completed native view-select SQL after
    the required `AS` token.

## Design

Reuse the existing metadata-only view recovery classification for the nested
check-option selectors. No parser change is required.

Upgrade the two existing hook selectors so a live peer remains open after the
writer is killed. A new ownerless opener must recover the nested views while
that peer remains open, verify inner and outer view metadata, exercise valid
DML through the outer view, verify inner and outer predicate failures, and keep
the native file-operation marker clear.

## Scope

In scope:

- Focused outer `CREATE OR REPLACE VIEW` replacement from `LOCAL` to
  `CASCADED` live recovery.
- Focused inner `ALTER VIEW` predicate rewrite while the outer cascaded view
  remains in place.
- Inner and outer `.frm` presence, `CHECK_OPTION`, and `IS_UPDATABLE` checks.
- Valid and invalid through-outer-view DML while another ownerless peer remains
  live.
- Ownerless/native reopen and forced `.shm` rebuild after final peer release.

Out of scope:

- Complex join views and non-updatable nested view diagnostics.
- Prepared nested view DML crash boundaries.
- Security/definer, invalid-definer, and invalid-dependency live recovery.
- Trigger, schema, and broader metadata-only DDL live recovery.
- SQL-level table-lock fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL surface is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB nested check-option metadata and
enforcement are visible to a new ownerless opener before every peer exits.

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
  - `dictionary-view-nested-check-option-outer-replace-crash`
  - `dictionary-view-nested-check-option-inner-alter-crash`
- Run the focused CTest entries for both selectors.
- Run adjacent check-option metadata-only live-recovery selectors.
- Run production embedded ownerless nested-view and broader DDL selectors.
- Run targeted ownerless DDL stress.
- Run production-build guards, `format-check`, and `git diff --check`.

## Verification Results

Collected on branch `ownerless-concurrency` after the implementation:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-nested-check-option-outer-replace-crash` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-nested-check-option-inner-alter-crash` passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-dictionary-view-nested-check-option-(outer-replace|inner-alter)-crash$' --output-on-failure` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-check-option-alter-crash` passed as an adjacent guard.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test view-nested-check-option` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test view-check-option` passed.
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

- A killed nested check-option view writer is recovered by a new ownerless
  opener while another ownerless peer remains live.
- The native file-operation checkpoint-needed marker stays clear for both
  metadata-only view forms.
- Outer replacement recovery exposes the outer view as `CASCADED`, rejects a
  row that violates the inner predicate, accepts a valid nested view write, and
  rejects a row that violates the outer predicate.
- Inner alteration recovery preserves the outer cascaded view, accepts a row at
  the new inner lower bound, and rejects rows below the altered inner predicate
  or above the outer predicate.
- Ownerless and ordinary native reopen observe the same recovered state before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This remains focused nested-view recovery, not a full oracle for every nested
  view shape.
- Complex joins, non-updatable diagnostics, prepared nested view DML,
  security/definer, idempotent/no-op, and invalid-dependency view drop recovery
  are covered by separate live-recovery slices.
- Trigger, schema, and broader metadata-only DDL live recovery remain planned.
- Longer external MariaDB/RQG-style stress remains planned after bounded
  recovery classes stop producing correctness fixes.
