# Ownerless View Check-Option Live Recovery

## Problem Statement

Ownerless view metadata live recovery covers simple view create/drop, focused
view rewrites, and focused explicit column-list variants while another
ownerless peer remains open. `WITH LOCAL/CASCADED CHECK OPTION` view crash
coverage still recovered after no-live cleanup even though MariaDB had already
written the native view definition and MyLite's focused view classifiers can
identify the completed statement.

This slice promotes focused check-option `CREATE VIEW`, `CREATE OR REPLACE
VIEW`, and `ALTER VIEW` forms to metadata-only live recovery.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `view_check_option` parses no option, `WITH CHECK OPTION`,
    `WITH CASCADED CHECK OPTION`, and `WITH LOCAL CHECK OPTION`.
  - `CREATE VIEW`, `CREATE OR REPLACE VIEW`, and `ALTER VIEW` route through
    `view_select`, so check-option clauses occur after `AS`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` records updatability and rejects check options on
    non-updatable views.
  - `mysql_register_view()` stores the view check option in the native view
    definition file.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::view_check_option()` raises `ER_VIEW_CHECK_FAILED` when DML
    through a view violates the active predicate.
- `packages/libmylite/src/database.cc`
  - Focused view recovery classifiers require the view identifier and `AS`
    token and leave trailing view-select/check-option SQL to MariaDB's already
    completed native statement.
  - View recovery kinds are metadata-only and do not require native
    file-operation checkpoint marker evidence.

## Design

Reuse the existing metadata-only view recovery classification for focused
check-option view DDL. No parser broadening is needed because the existing
classifiers already accept trailing SQL after `AS`.

Upgrade the three existing hook selectors so a peer stays live after the writer
is killed. A new ownerless opener must recover the view while that peer remains
open, verify `CHECK_OPTION` and `IS_UPDATABLE`, exercise valid through-view
DML, prove invalid through-view DML still raises MariaDB errno 1369, and leave
the native file-operation marker clear.

## Scope

In scope:

- Focused `CREATE VIEW ... WITH CASCADED CHECK OPTION` live recovery.
- Focused `CREATE OR REPLACE VIEW ... WITH LOCAL CHECK OPTION` live recovery.
- Focused `ALTER VIEW ... WITH CASCADED CHECK OPTION` live recovery.
- Metadata checks through `INFORMATION_SCHEMA.VIEWS`.
- Valid and invalid through-view DML while another ownerless peer remains live.
- Ownerless/native reopen and forced `.shm` rebuild after final peer release.

Out of scope:

- Nested view local-versus-cascaded propagation live recovery.
- Non-updatable view diagnostics and prepared statement crash boundaries.
- Security/definer and invalid-definer live recovery.
- Trigger, schema, and broader metadata-only DDL live recovery.
- SQL-level table-lock fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL surface is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB check-option metadata and enforcement are
visible to a new ownerless opener before every peer exits.

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
  - `dictionary-view-check-option-create-crash`
  - `dictionary-view-check-option-replace-crash`
  - `dictionary-view-check-option-alter-crash`
- Run the focused CTest entries for the three selectors.
- Run adjacent view metadata-only live-recovery selectors.
- Run production embedded ownerless view and broader DDL selectors.
- Run targeted ownerless DDL stress.
- Run production-build guards, `format-check`, and `git diff --check`.

## Acceptance Criteria

- A killed check-option view writer is recovered by a new ownerless opener
  while another ownerless peer remains live.
- The native file-operation checkpoint-needed marker stays clear for all three
  metadata-only view forms.
- Create recovery exposes `CHECK_OPTION='CASCADED'`, `IS_UPDATABLE='YES'`,
  queryable view rows, valid through-view DML, and errno 1369 for invalid DML.
- Replacement recovery exposes `CHECK_OPTION='LOCAL'`, `IS_UPDATABLE='YES'`,
  the replacement predicate, valid through-view DML, and errno 1369 for invalid
  DML.
- Alter recovery exposes `CHECK_OPTION='CASCADED'`, `IS_UPDATABLE='YES'`, the
  altered predicate, valid through-view DML, and errno 1369 for invalid DML.
- Ownerless and ordinary native reopen observe the same recovered state before
  and after forced `.shm` rebuild.

## Verification Results

- Hook build: `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`.
- Hook selectors:
  - `dictionary-view-check-option-create-crash`
  - `dictionary-view-check-option-replace-crash`
  - `dictionary-view-check-option-alter-crash`
- Hook CTest:
  `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-dictionary-view-check-option-(create|replace|alter)-crash$' --output-on-failure`.
- Adjacent hook selectors:
  - `dictionary-view-column-list-create-crash`
  - `dictionary-view-replace-crash`
- Production embedded build:
  `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`.
- Production selectors:
  - `view-check-option`
  - `view-ddl`
  - `ddl-broader`
- Targeted stress:
  - `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
  - `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`

## Risks And Follow-Up

- This remains focused SQL-shape recovery, not a complete parser or oracle for
  every legal updatable view definition.
- Nested check-option propagation, non-updatable diagnostics, security/definer,
  and idempotent/no-op view variants are covered by separate live-recovery
  slices; invalid-dependency view drop recovery is covered by
  `ownerless-view-invalid-dependency-drop-live-recovery`.
- Trigger, schema, and broader metadata-only DDL live recovery remain planned.
- Longer external MariaDB/RQG-style stress remains planned after bounded
  recovery classes stop producing correctness fixes.
