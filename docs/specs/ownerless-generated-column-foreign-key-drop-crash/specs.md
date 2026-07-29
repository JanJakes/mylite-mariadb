# Ownerless Generated-Column Foreign-Key DROP Crash

## Problem Statement

Ownerless generated-column foreign-key crash coverage proves recovery when a
writer dies after native InnoDB creates representative stored generated-column
FK metadata but before MyLite publishes ownerless dictionary finish. Ordinary
foreign-key DROP crash coverage proves the adjacent non-generated DROP FOREIGN
KEY boundary.

The remaining intersection is crash recovery for successful DROP FOREIGN KEY on
stored generated-column FK shapes. A writer can finish MariaDB/InnoDB metadata
removal for a generated-column FK, then die before
`ownerless_finish_dictionary_ddl()`. MyLite must recover the completed native
constraint removal without resurrecting stale FK metadata or enforcement.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:9570-9610` leaves `DROP FOREIGN KEY` entries in
  the ALTER drop list and validates named child-side FK existence before the
  storage engine handles the ALTER.
- `mariadb/storage/innobase/handler/handler0alter.cc:4421-4440` identifies
  InnoDB foreign keys selected for drop during ALTER validation.
- `mariadb/storage/innobase/handler/handler0alter.cc:8275-8340` resolves named
  `ALTER_DROP_FOREIGN_KEY` operations against the table foreign-key set.
- `mariadb/storage/innobase/handler/handler0alter.cc:9371-9418` deletes a
  dropped FK from `SYS_FOREIGN` and `SYS_FOREIGN_COLS`.
- `mariadb/storage/innobase/handler/handler0alter.cc:10046-10130` applies
  InnoDB FK additions and drops to persistent dictionary tables before cache
  refresh.
- `mariadb/storage/innobase/handler/handler0alter.cc:10133-10185` refreshes
  InnoDB foreign-key cache state after native dictionary updates.
- Existing generated-column FK source findings remain authoritative for
  accepted stored generated-column FK shapes:
  `mariadb/storage/innobase/dict/dict0crea.cc:1685-1728`,
  `mariadb/storage/innobase/handler/handler0alter.cc:3170-3200`,
  `mariadb/storage/innobase/handler/handler0alter.cc:3246-3415`,
  `mariadb/storage/innobase/dict/dict0mem.cc:959-988`, and
  `mariadb/storage/innobase/row/row0ins.cc:901-935`.
- `packages/libmylite/src/database.cc:2939-3007` brackets direct ownerless SQL
  with dictionary begin/finish handling, and
  `packages/libmylite/src/database.cc:9299-9308` runs the unsafe
  `dictionary-before-finish` hook after native SQL succeeds and before MyLite
  marks ownerless dictionary DDL complete.
- `packages/libmylite/src/database.cc:20080-20160` classifies bounded
  generated-column FK DROP statements by proving the existing named FK metadata
  involves a generated child or referenced column.
- MyLite prearms bounded FK recovery because even a metadata-only native
  dictionary change needs startup authority after a crash. MariaDB's actual
  native file-operation callback remains an additional authoritative signal
  when the selected generated-column FK ALTER rebuilds a tablespace.

## Design

Add one hook-only selector,
`dictionary-generated-column-foreign-key-drop-crash`, to
`mylite_ownerless_cross_process_sql_test`.

The selector initializes one ownerless database with two already-created
generated-column FK shapes:

1. A child table with a stored generated `parent_key` column referencing a
   regular parent primary key.
2. A parent table with a unique stored generated `parent_key` column referenced
   by a regular child column.

For each shape, the selector first proves the constraint is present and
enforced. It then runs an ownerless writer under the existing
`dictionary-before-finish` hook, kills the writer after native DROP FOREIGN KEY
completion, opens recovery while the live peer remains open, and verifies the
completed native metadata removal before releasing the peer. The selector
asserts that MariaDB's actual file-operation callback retains the structural
marker through live recovery for the generated child-column DROP. The generated
referenced-column DROP takes MariaDB's metadata-only path, but retains its
prearmed native-dictionary marker across the crash for the same startup
authority. Final no-live peer shutdown drains either marker when the tablespace
identity remains unchanged.

After recovery, verify:

- `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS` and `KEY_COLUMN_USAGE` entries
  for the dropped constraints are absent,
- generated columns and supporting indexes remain readable,
- orphan child inserts now succeed,
- parent deletes that would have been restricted or cascaded now succeed
  without deleting existing children,
- ownerless/native reopen before and after forced `.shm` rebuild observe the
  same final state.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` recovery for a generated child-column FK
  DROP,
- crash-at-`dictionary-before-finish` recovery for a generated referenced-column
  FK DROP,
- live-peer recovery with marker retention for both physical and metadata-only
  native dictionary changes,
- recovered absence of generated-column FK metadata and enforcement,
- ownerless/native reopen before and after forced shared-memory rebuild.

Out of scope:

- crash injection while FK referential actions themselves execute,
- virtual generated child-column FK creation beyond existing policy coverage,
- MariaDB-rejected generated-column action clauses,
- long-running randomized FK graph or RQG stress,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens the ownerless
generated-column and foreign-key claims by proving representative completed
native generated-column FK DROP metadata removal survives a writer crash at
MyLite's dictionary publication boundary.

The coverage remains partial: post-child-action crash-in-action execution,
broader generated FK matrices, and randomized external FK stress remain
planned. Pre-child-action generated-column FK action crash recovery is covered
by `docs/specs/ownerless-generated-column-foreign-key-action-crash/specs.md`.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector exercises existing
ownerless process-slot cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle inside the MyLite
database directory.

## Native Storage Impact

No InnoDB format change. MyLite continues to rely on MariaDB/InnoDB native
generated-column and foreign-key metadata. The test proves ownerless recovery
rebuilds volatile coordination around completed native metadata removal.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-generated-column-foreign-key-drop-crash`.
- Run adjacent hook coverage:
  `dictionary-generated-column-foreign-key-crash`,
  `dictionary-foreign-key-drop-crash`, and
  `dictionary-generated-column-success-crash`.
- Run adjacent non-hook generated/FK selectors:
  `generated-column-foreign-key`,
  `generated-column-foreign-key-policy`, and `foreign-key-ddl`.
- Run registered ownerless hook CTest, embedded ownerless SQL shards,
  ownerless stress, `format-check`, `tidy`, and `git diff --check`.

## Acceptance Criteria

- Each killed DROP writer reaches `dictionary-before-finish` without hanging.
- A live ownerless peer remains open while recovery exposes each completed FK
  DROP.
- The generated child-column FK DROP marker remains set through live recovery
  and clears when the final peer closes with unchanged tablespace identity.
  The generated referenced-column FK DROP retains its prearmed marker through
  live recovery even though MariaDB emits no native file operation, then clears
  at the same final no-live boundary.
- Recovered generated child-column FK metadata is absent and orphan writes are
  accepted.
- Recovered generated referenced-column FK metadata is absent and parent deletes
  no longer cascade or restrict existing child rows.
- Generated values and supporting generated-column indexes remain readable.
- Ownerless and ordinary native reopen observe the same final state before and
  after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic crash coverage for two accepted FK DROP shapes, not an
  exhaustive generated-column FK matrix.
- Crash injection during FK action execution remains separate recovery work.
- Full external MariaDB/RQG FK stress remains environment-owned follow-up work.
