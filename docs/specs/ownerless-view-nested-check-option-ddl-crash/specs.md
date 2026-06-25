# Ownerless Nested View Check-Option DDL Crash

## Problem Statement

Ownerless nested view check-option coverage proves already-open peers refresh
outer `LOCAL` versus `CASCADED` propagation and inner predicate alterations.
It did not yet prove those nested view rewrites survive a writer death after
MariaDB has rewritten the native view definition file and before MyLite
publishes ownerless dictionary finish.

This slice adds deterministic crash-boundary evidence for nested view
check-option propagation. The follow-up
[ownerless-view-nested-check-option-live-recovery](../ownerless-view-nested-check-option-live-recovery/specs.md)
promotes the focused nested outer replacement and inner alter forms to
metadata-only live-peer recovery.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `ALTER VIEW` and `CREATE OR REPLACE VIEW` both parse `view_select`, which
    consumes `view_check_option`.
  - `view_check_option` accepts no option, `WITH CHECK OPTION`,
    `WITH CASCADED CHECK OPTION`, and `WITH LOCAL CHECK OPTION`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` handles create, replacement, and alter view commands.
  - `mysql_make_view()` stores `TABLE_LIST::effective_with_check` through
    `LEX::get_effective_with_check()`.
  - `mysql_register_view()` stores `view->with_check` in the native view
    definition.
- `mariadb/sql/sql_lex.cc`
  - `LEX::get_effective_with_check()` returns the stored view check-option mode
    for the applicable top-level view.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::prep_check_option()` passes `VIEW_CHECK_CASCADED` into
    underlying views only when the outer view is cascaded; an outer local view
    passes `VIEW_CHECK_NONE` to underlying views.
  - `TABLE_LIST::view_check_option()` raises `ER_VIEW_CHECK_FAILED` on
    predicate violation.
- `mariadb/sql/sql_insert.cc` and `mariadb/sql/sql_update.cc`
  - DML through updatable views calls `view_check_option()`.
- `packages/libmylite/src/database.cc`
  - `ownerless_dictionary_ddl_statement()` classifies `CREATE` and `ALTER` as
    ownerless dictionary DDL.
  - `ownerless_finish_dictionary_ddl()` exposes the unsafe
    `dictionary-before-finish` hook used by other DDL crash tests.

## Design

Add two unsafe-hook selectors to `mylite_ownerless_cross_process_sql_test`:

- `dictionary-view-nested-check-option-outer-replace-crash` creates an InnoDB
  base table, an inner `CASCADED` check-option view, and an outer `LOCAL`
  check-option view. It kills a writer after `CREATE OR REPLACE VIEW` rewrites
  the outer view to `CASCADED`, then verifies recovered nested metadata and
  cascaded inner-predicate enforcement.
- `dictionary-view-nested-check-option-inner-alter-crash` creates an InnoDB base
  table, an inner `CASCADED` check-option view, and an outer `CASCADED`
  check-option view. It kills a writer after `ALTER VIEW` rewrites the inner
  predicate from `value >= 10` to `value >= 8`, then verifies recovered nested
  metadata and the altered inner-predicate enforcement through the outer view.

Both selectors verify metadata-only live recovery while another ownerless peer
remains open, then ownerless and ordinary native reopen before and after a
forced `.shm` rebuild.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for outer nested view
  replacement from `LOCAL` to `CASCADED`.
- Crash-at-`dictionary-before-finish` coverage for inner nested view predicate
  alteration while the outer cascaded view remains in place.
- Recovered inner and outer `.frm` files under `datadir/app/`.
- Recovered `INFORMATION_SCHEMA.VIEWS.CHECK_OPTION` and `IS_UPDATABLE`
  metadata for both nested views.
- Query behavior and valid DML through the recovered outer updatable view.
- Invalid insert/update failure with MariaDB errno 1369 for inner and outer
  predicate violations.
- Live-peer recovery without native file-operation marker evidence.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Complex join views, non-updatable views, invalid dependencies, prepared
  nested view DML, `SQL SECURITY`, privilege behavior, invalid definers, and
  routine interaction.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB nested check-option view rewrites survive
a writer death at MyLite's dictionary publication boundary and continue to
enforce MariaDB-compatible DML errors.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise native MariaDB
view `.frm` files inside the MyLite-owned database directory, ownerless
live-peer recovery, final no-live cleanup, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

The base tables are InnoDB. The slice verifies base-table durability and nested
updatable view DML after recovery, but it does not alter InnoDB storage formats,
page-version replay policy, redo/checkpoint policy, or durable file-lifecycle
metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-nested-check-option-outer-replace-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-nested-check-option-inner-alter-crash`
- Run the normal embedded `view-nested-check-option` selector.
- Run the relevant ownerless hook SQL shard and embedded ownerless SQL shard.
- Run DDL stress, `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer remains open while a new ownerless opener recovers the completed
  nested view metadata and check-option propagation state.
- The native file-operation marker remains clear for these metadata-only view
  forms.
- Outer replacement recovery exposes the outer view as `CASCADED`, rejects a
  row that violates the inner predicate, accepts a valid nested view write, and
  rejects a row that violates the outer predicate.
- Inner alteration recovery preserves the outer cascaded view, accepts a row at
  the new inner lower bound, and rejects rows below the altered inner predicate
  or above the outer predicate.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic nested check-option replacement and alteration
  boundaries, not every nested view semantic.
- Complex joins, invalid dependencies, non-updatable diagnostics, prepared
  nested view DML, randomized view oracles, and external long-running DDL stress
  remain planned.
