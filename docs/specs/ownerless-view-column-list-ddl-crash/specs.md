# Ownerless View Column-List DDL Crash

## Problem Statement

Ownerless view column-list refresh coverage proves already-open peers observe
explicit view column aliases across `CREATE VIEW`, `CREATE OR REPLACE VIEW`,
and `ALTER VIEW`. Hook-build crash coverage covers simple view
create/drop, idempotent no-ops, replacement/altered definitions, and bounded
security metadata, but it does not yet prove that explicit column-list metadata
survives a writer death after MariaDB writes the native view definition file and
before MyLite publishes ownerless dictionary finish.

This slice adds deterministic crash-boundary evidence for explicit column-list
create, replace, and alter paths. The follow-up
[ownerless-view-column-list-live-recovery](../ownerless-view-column-list-live-recovery/specs.md)
promotes those focused forms to metadata-only live-peer recovery.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `CREATE VIEW`, `CREATE OR REPLACE VIEW`, and `ALTER VIEW` all accept
    `view_list_opt` before `AS view_select`.
  - `view_list` pushes explicit aliases into `Lex->view_list`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` checks `lex->view_list` arity against the select
    item list, assigns each explicit alias to the corresponding item, and marks
    the item with `IS_EXPLICIT_NAME`.
  - The stored view text builder includes `lex->view_list` aliases in the view
    definition text.
  - `mysql_register_view()` writes the native view definition through
    `sql_create_definition_file()` and removes the view from the
    table-definition cache after successful registration.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `CREATE`, `ALTER`, `DROP`,
    and `RENAME` statements as dictionary-generation boundaries.
  - The unsafe hook `dictionary-before-finish` fires after the native DDL
    statement returns and before ownerless dictionary generation is published.

## Design

Add three unsafe-hook selectors to
`mylite_ownerless_cross_process_sql_test`:

- `dictionary-view-column-list-create-crash` creates an InnoDB base table,
  kills a writer after `CREATE VIEW ... (view_id, view_value, view_note)` writes
  the native view definition but before ownerless dictionary finish, then
  verifies recovered explicit column aliases and view query behavior.
- `dictionary-view-column-list-replace-crash` creates an initial explicit
  column-list view, kills a writer after `CREATE OR REPLACE VIEW ... (view_id,
  adjusted_value, label)` rewrites the native view definition, then verifies the
  replacement aliases and predicate.
- `dictionary-view-column-list-alter-crash` creates an initial explicit
  column-list view, kills a writer after `ALTER VIEW ... (view_id,
  doubled_value, label)` rewrites the native view definition, then verifies the
  altered aliases and predicate.

Each selector verifies metadata-only live recovery while another ownerless peer
remains open, then ownerless and ordinary native reopen before and after a
forced `.shm` rebuild.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for explicit column-list view
  creation.
- Crash-at-`dictionary-before-finish` coverage for explicit column-list
  `CREATE OR REPLACE VIEW`.
- Crash-at-`dictionary-before-finish` coverage for explicit column-list
  `ALTER VIEW`.
- Recovered `.frm` presence under `datadir/app/`.
- Recovered `INFORMATION_SCHEMA.VIEWS` and `INFORMATION_SCHEMA.COLUMNS`
  metadata, including alias names and ordinal positions.
- Query behavior proving stale exposed column names disappear after rewrites.
- Base-table writes after recovery.
- Live-peer recovery without native file-operation marker evidence.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Invalid column-list arity or invalid dependencies.
- Updatable views, check-option variants, nested-view variants, or
  security/definer variants.
- SQL-level table-lock fault injection for native table-wait paths.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB explicit view column-list metadata changes
survive a writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise native MariaDB
view `.frm` files inside the MyLite-owned database directory, ownerless
live-peer recovery, final no-live cleanup, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

The base tables are InnoDB. The slice verifies base-table durability and view
query behavior after recovery, but it does not alter InnoDB storage formats,
page-version replay policy, redo/checkpoint policy, or durable file-lifecycle
metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-column-list-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-column-list-replace-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-column-list-alter-crash`
- Run the normal embedded `view-column-list` selector.
- Run adjacent focused view crash selectors and the relevant ownerless hook SQL
  shard.
- Run DDL stress, `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer remains open while a new ownerless opener recovers the completed
  native view metadata.
- The native file-operation marker remains clear for these metadata-only view
  forms.
- Create recovery exposes `view_id`, `view_value`, and `view_note` metadata in
  ordinal order and queries the created view.
- Replacement recovery exposes `adjusted_value`, rejects the old `view_value`
  name, and queries the replacement predicate/projection.
- Alter recovery exposes `doubled_value`, rejects the old `view_value` name,
  and queries the altered predicate/projection.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic explicit column-list create, replace, and alter
  boundaries, not every view semantic.
- Invalid column-list arity, invalid dependency recovery, randomized view
  oracles, and external long-running DDL stress remain planned.
