# Ownerless View Invalid Dependency Drop Live Recovery

## Problem Statement

Ownerless view diagnostics already prove an already-open peer observes a valid
view becoming invalid after its base table is dropped, reports MariaDB errno
1356, and becomes valid again when the base table is recreated. The remaining
metadata-only live-recovery gap is the crash boundary where a writer drops an
already-invalid view after MariaDB removes the native view definition but before
MyLite publishes ownerless dictionary finish.

MyLite must finish that metadata-only boundary while another ownerless peer
remains live, without requiring native file-operation checkpoint evidence and
without recreating the dropped view when the base table is later recreated.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/libmariadb/include/mysqld_error.h`
  - `ER_VIEW_INVALID` is MariaDB errno 1356.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` resolves the view query before writing the view
    definition, so this slice does not claim MariaDB accepts direct creation of
    a view over a missing base table.
  - `mysql_drop_view()` removes the native view definition file for accepted
    `DROP VIEW`.
- `mariadb/sql/sql_base.cc`
  - View dependency lookup failures for a table that belongs to a view are
    converted to `ER_VIEW_INVALID`.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::replace_view_error_with_generic()` rewrites missing
    dependency errors to `ER_VIEW_INVALID` for the top view.
- `packages/libmylite/src/database.cc`
  - `ownerless_drop_view_recovery_statement()` classifies focused `DROP VIEW`
    and optional `IF EXISTS` forms.
  - `ownerless_dictionary_recovery_kind_is_metadata_only()` marks
    `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_VIEW` as metadata-only, so
    live-peer recovery should not require a native file-operation marker.

## Design

Add a hook-build selector,
`dictionary-view-invalid-dependency-drop-crash`, to
`mylite_ownerless_cross_process_sql_test`.

The selector:

1. Creates an InnoDB base table and a valid view over that table.
2. Drops the base table, leaving the native view definition present and
   verifying queries through the view fail with MariaDB errno 1356.
3. Keeps a live ownerless peer open.
4. Kills a writer after native `DROP VIEW` removes the invalid view definition
   but before ownerless dictionary finish.
5. Opens a new ownerless handle while the peer remains live, proving
   metadata-only recovery completes and the native file-operation marker stays
   clear.
6. Releases the held peer, recreates the base table under the same name, and
   verifies the dropped view remains absent.
7. Verifies ownerless and ordinary native reopen before and after forced `.shm`
   rebuild preserve the absent view and recreated base table.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for dropping an already-invalid
  view.
- Live-peer metadata-only ownerless recovery of absent view state.
- MariaDB errno 1356 evidence before the crash.
- Native `.frm` absence for the dropped view and InnoDB `.ibd` presence for the
  recreated base table.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Direct creation of a view over a missing base table.
- Invalid definers, privilege/security failures, stored functions, nested
  invalid dependencies, complex join/union views, and randomized view oracles.
- Trigger, schema, table, index, and foreign-key live-recovery breadth.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless compatibility
evidence by proving MariaDB-native invalid-view diagnostics and subsequent
metadata-only view drop semantics survive a writer death at the MyLite
dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector observes native view
metadata under `datadir/app/*.frm` and base-table InnoDB state under
`datadir/app/*.ibd`, then verifies ownerless/native reopen and forced `.shm`
rebuild agree on the final directory state.

## Native Storage Impact

The base table is InnoDB, but the promoted recovery boundary is metadata-only
view removal. The slice does not change InnoDB page-version WAL, redo/checkpoint
policy, page flushing, or native tablespace file lifecycle.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused hook selector:
  - `dictionary-view-invalid-dependency-drop-crash`
- Run the focused CTest entry.
- Run adjacent metadata-only view live-recovery selectors.
- Run production embedded invalid-dependency and broader view/DDL selectors.
- Run targeted ownerless DDL stress.
- Run production-build guards, `format-check`, and `git diff --check`.

## Verification Results

Collected on branch `ownerless-concurrency` after the implementation:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-invalid-dependency-drop-crash` passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-view-invalid-dependency-drop-crash$' --output-on-failure` passed.
- `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-dictionary-view-(create|drop|replace|alter|column-list-(create|replace|alter)|check-option-(create|replace|alter)|nested-check-option-(outer-replace|inner-alter)|security-(create|replace|alter)|idempotent-(create|drop)|invalid-dependency-drop)-crash$' --output-on-failure` passed.
- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test view-invalid-dependency` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test view-ddl` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test ddl-broader` passed.
- `cmake --build --preset ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2` passed.
- `ctest --preset ownerless-stress -R '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure` passed on clean rerun in `149.72s`. During verification, the same stress target first timed out once at `900.42s` with no test output, then after source formatting failed once with the existing statement-lock retry exhaustion class (`ownerless dictionary statement lock is busy`) before a clean rerun passed.
- `cmake --build --preset prod --target format-check` passed.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure` passed.
- `git diff --check` passed.
- Cleanup scans found no `/tmp/mylite-ownerless-*` directories and no ownerless test processes.

## Acceptance Criteria

- The selector reaches the dictionary fault hook and does not hang.
- A new ownerless opener recovers the invalid-view drop while another ownerless
  peer remains live.
- The native file-operation checkpoint-needed marker stays clear for the
  metadata-only view drop.
- Recreating the dropped base table does not reanimate the dropped view.
- Ownerless and ordinary native reopen observe the same absent-view and
  recreated-base-table state before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers a deterministic invalid-dependency view drop boundary, not every
  invalid dependency source or complex view form.
- Broader metadata-only DDL live recovery for trigger and schema classes remains
  planned.
- Multi-table/cross-schema dropped tablespace live recovery and broader rebuild
  variants remain planned.
- Longer external MariaDB/RQG-style stress remains planned after bounded
  recovery classes stop producing correctness fixes.
