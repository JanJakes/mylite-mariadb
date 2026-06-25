# Ownerless View Metadata Live Recovery

## Problem Statement

Ownerless simple view crash coverage kills a writer after MariaDB creates or
drops the native view definition file but before MyLite publishes dictionary
finish. That coverage previously proved no-live recovery only: another
ownerless opener stayed busy while a peer process remained open. Supported view
DDL should not require all peers to exit after a writer dies at this completed
metadata boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_VIEW` to
  `mysql_create_view()` and `SQLCOM_DROP_VIEW` to `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc::mysql_register_view()` writes the native view
  definition file through `sql_create_definition_file()`.
- `mariadb/sql/sql_view.cc::mysql_drop_view()` removes the native view
  definition file and invalidates table-definition cache state.
- `packages/libmylite/src/database.cc` currently marks recoverable dictionary
  state before the `dictionary-before-finish` hook only when native InnoDB
  file-operation checkpoint evidence was written. Simple view DDL has durable
  SQL-layer metadata but does not require InnoDB file-operation redo evidence.

## Scope And Non-Goals

In scope:

- Add metadata-only ownerless dictionary recovery kinds for simple
  `CREATE VIEW schema.view AS ...` and `DROP VIEW schema.view`.
- Mark those metadata-only kinds recoverable after native execution and before
  the dictionary-finish hook without requiring native file-operation checkpoint
  evidence.
- Let dead-owner cleanup recover only metadata-only kinds when no native
  file-operation marker is present; file-operation DDL remains gated by the
  existing marker.
- Upgrade `dictionary-view-create-crash` and `dictionary-view-drop-crash` to
  prove live-peer recovery.

Out of scope:

- `CREATE OR REPLACE VIEW`, `ALTER VIEW`, idempotent view no-op DDL, view column
  lists, check-option views, SQL security/definer variants, nested views,
  triggers, routines, schema DDL, and table metadata-only ALTER variants.
- Any change to MariaDB view semantics or native view file format.
- SQL-level table-lock fault injection.

## Design

Extend the recoverable dictionary-state kind set with:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_VIEW`
- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_VIEW`

The statement classifiers intentionally stay narrow:

- `CREATE VIEW <one-or-two-part-name> AS ...`
- `DROP VIEW <one-or-two-part-name>`

They reject `OR REPLACE`, `IF [NOT] EXISTS`, multi-view drops, and broader view
syntax. Those variants already have separate crash selectors but remain outside
this first metadata-only live-recovery lane.

`ownerless_finish_dictionary_ddl()` should write a recoverable dictionary marker
when either:

- the existing native file-operation marker was written, or
- the recovery kind is metadata-only.

`ownerless_process_recover_dead_dictionary_owner()` should still require native
file-operation marker evidence before attempting file-operation recovery kinds.
When no marker is present, it should attempt only metadata-only recovery kinds.
This preserves the proof boundary for table DDL while allowing simple view DDL
to finish the dead owner's dictionary generation while a peer remains live.

The tests keep a live peer open after killing the view writer, open another
ownerless handle while that peer is live, and verify recovered view metadata.
They also assert that the native file-operation checkpoint marker remains clear
for this metadata-only path.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless view
compatibility by proving simple completed CREATE/DROP view metadata can recover
with a live peer instead of requiring all peers to exit.

## Directory And Lifecycle Impact

No directory layout changes. The slice uses existing dictionary-generation
state in `concurrency/mylite-concurrency.shm` and MariaDB-native view `.frm`
files under `datadir/<schema>/`.

## Native Storage Impact

No InnoDB storage format changes. The base table remains InnoDB for query and
post-recovery DML checks, but the recovered DDL boundary is SQL-layer view
metadata.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds two recovery-kind values, two bounded
classifiers, focused tests, and docs.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selectors:
  `dictionary-view-create-crash` and `dictionary-view-drop-crash`.
- Run adjacent view hook selectors:
  `dictionary-view-idempotent-create-crash` and
  `dictionary-view-idempotent-drop-crash`.
- Run registered hook CTest entries for the two simple view selectors.
- Build production ownerless SQL target and run representative non-hook
  `view-ddl` and `ddl-broader` selectors.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- A killed simple `CREATE VIEW` writer leaves recoverable metadata-only
  dictionary state and no native file-operation marker.
- A live ownerless peer can remain open while a new ownerless opener recovers
  the created view and queries it.
- A killed simple `DROP VIEW` writer leaves recoverable metadata-only
  dictionary state and no native file-operation marker.
- A live ownerless peer can remain open while a new ownerless opener recovers
  the absent view and keeps the base table writable.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  preserve the recovered states.

## Verification Results

- Hook build: `mylite_ownerless_cross_process_sql_test`.
- Hook selectors: `dictionary-view-create-crash` and
  `dictionary-view-drop-crash`.
- Adjacent hook selectors: `dictionary-view-idempotent-create-crash`,
  `dictionary-view-idempotent-drop-crash`, `dictionary-view-replace-crash`, and
  `dictionary-trigger-definer-crash`.
- Hook CTest:
  `libmylite.ownerless-dictionary-view-create-crash` and
  `libmylite.ownerless-dictionary-view-drop-crash`.
- Production embedded build: `mylite_ownerless_cross_process_sql_test`.
- Production selectors: `view-ddl` and `ddl-broader`.

## Risks And Follow-Up

- Metadata-only live recovery is intentionally scoped to simple view
  CREATE/DROP first. Broader view variants, triggers, schema metadata, and
  table metadata-only ALTER classes need separate bounded specs.
- The metadata-only lane must not allow file-operation DDL to recover without
  durable native marker evidence.
- External MariaDB/RQG stress remains planned after bounded recovery gates stop
  producing new correctness issues.
