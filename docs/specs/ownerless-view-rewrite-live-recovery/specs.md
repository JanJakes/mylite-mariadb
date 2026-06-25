# Ownerless View Rewrite Live Recovery

## Problem Statement

Simple `CREATE VIEW` and `DROP VIEW` now use metadata-only recoverable
dictionary markers so a new ownerless opener can finish a dead writer's
dictionary generation while another peer remains live. `CREATE OR REPLACE VIEW`
and simple `ALTER VIEW` rewrite an existing native view definition file and
previously still required all peers to exit after a crash at
`dictionary-before-finish`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_VIEW` for both
  `CREATE OR REPLACE VIEW` and `ALTER VIEW` through `mysql_create_view()`.
- `mariadb/sql/sql_view.cc::mysql_create_view()` handles
  `VIEW_CREATE_OR_REPLACE` and `VIEW_ALTER`.
- `mariadb/sql/sql_view.cc::mysql_register_view()` validates the existing view,
  backs up the old definition file, writes the new definition through
  `sql_create_definition_file()`, and removes table-definition cache state.
- MyLite's metadata-only recovery lane already distinguishes view metadata
  recovery from table DDL recovery that requires native file-operation
  checkpoint evidence.

## Scope And Non-Goals

In scope:

- Add metadata-only recovery kinds for focused
  `CREATE OR REPLACE VIEW <view> AS ...` and `ALTER VIEW <view> AS ...`.
- Upgrade `dictionary-view-replace-crash` and `dictionary-view-alter-crash` to
  prove live-peer recovery.
- Prove rewritten column metadata and query behavior while the original peer
  remains live.
- Prove the native file-operation checkpoint marker stays clear.

Out of scope:

- Column-list view rewrites, check-option views, nested views,
  SQL-security/definer variants, invalid dependencies, updatable views,
  triggers, schema metadata, and table metadata-only ALTER variants.
- Broadening simple `CREATE VIEW`/`DROP VIEW` recovery beyond the prior
  metadata-only live-recovery slice.

## Design

Add two metadata-only ownerless dictionary recovery kinds:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_OR_REPLACE_VIEW`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_VIEW`

The original rewrite slice constrained the base classifiers to:

- `CREATE OR REPLACE VIEW <one-or-two-part-name> AS ...`
- `ALTER VIEW <one-or-two-part-name> AS ...`

Follow-up live-recovery slices broaden the shared view classifiers for explicit
column lists, check-option, nested check-option, and security/definer variants.
Invalid-dependency view recovery remains separate planned work.

The existing metadata-only finish and cleanup path should mark these kinds
recoverable after native execution and recover them without native
file-operation marker evidence. File-changing table DDL continues to require
the native marker before dead-owner recovery.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless view
compatibility by proving completed MariaDB view definition rewrites can recover
with a live peer.

## Directory And Lifecycle Impact

No directory layout changes. The slice exercises MariaDB-native view `.frm`
rewrite state under `datadir/app/` plus existing MyLite dictionary-generation
state in the database directory.

## Native Storage Impact

No InnoDB storage-format changes. InnoDB base tables are used only as stable
view query oracles before and after metadata recovery.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds two recovery-kind values, two bounded
classifiers, focused tests, CTest registration, and docs.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selectors:
  `dictionary-view-replace-crash` and `dictionary-view-alter-crash`.
- Run adjacent view hook selectors:
  `dictionary-view-create-crash`, `dictionary-view-drop-crash`,
  `dictionary-view-idempotent-create-crash`, and
  `dictionary-view-idempotent-drop-crash`.
- Run registered hook CTest entries for replacement and alter selectors.
- Build production ownerless SQL target and run representative non-hook
  `view-ddl` and `ddl-broader` selectors.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- A killed `CREATE OR REPLACE VIEW` writer recovers while a peer remains live,
  exposes the `adjusted` projection, rejects the old `value` projection, and
  leaves the native file-operation marker clear.
- A killed `ALTER VIEW` writer recovers while a peer remains live, exposes the
  `doubled` projection, rejects the old `value` projection, and leaves the
  native file-operation marker clear.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  preserve the rewritten view definitions.

## Verification Results

- Hook build: `mylite_ownerless_cross_process_sql_test`.
- Hook selectors: `dictionary-view-replace-crash` and
  `dictionary-view-alter-crash`.
- Adjacent hook selectors: `dictionary-view-create-crash`,
  `dictionary-view-drop-crash`, `dictionary-view-idempotent-create-crash`,
  `dictionary-view-idempotent-drop-crash`, and
  `dictionary-view-column-list-replace-crash`.
- Hook CTest:
  `libmylite.ownerless-dictionary-view-replace-crash` and
  `libmylite.ownerless-dictionary-view-alter-crash`.
- Production embedded build: `mylite_ownerless_cross_process_sql_test`.
- Production selectors: `view-ddl` and `ddl-broader`.

## Risks And Follow-Up

- This promotes only the focused no-column-list rewrite forms. Column-list,
  check-option, nested, and security/definer view variants are covered by
  follow-up metadata-only live-recovery slices; invalid-dependency view variants
  remain planned.
- Trigger, schema, and table metadata-only live recovery remain planned.
- External MariaDB/RQG stress remains planned after bounded recovery gates stop
  producing new correctness issues.
