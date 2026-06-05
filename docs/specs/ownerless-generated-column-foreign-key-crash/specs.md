# Ownerless Generated-Column Foreign-Key Crash

## Problem Statement

Ownerless generated-column foreign-key coverage proves peer refresh,
generated values, referential enforcement, cascaded deletes, and reopen
durability for representative MariaDB-supported stored generated-column FK
shapes. Ordinary foreign-key hook coverage also proves recovery when a writer
dies after native FK metadata creation or removal but before MyLite publishes
the ownerless dictionary generation.

The remaining intersection is crash recovery for successful generated-column
foreign-key DDL. A writer can finish MariaDB/InnoDB FK metadata creation for a
generated-column FK, then die before `ownerless_finish_dictionary_ddl()`.
MyLite must recover the completed native metadata and preserve generated values
and referential enforcement without requiring the crashed writer to finish.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/field.cc:11344-11354` rejects non-strictly deterministic
  generated columns before they can be used as key parts.
- `mariadb/storage/innobase/dict/dict0crea.cc:1685-1728` rejects FK actions
  on base columns of stored generated columns when the action would mutate
  generated-column consistency.
- `mariadb/storage/innobase/handler/handler0alter.cc:3170-3200` implements the
  parallel ALTER-time base-column guard for stored generated columns.
- `mariadb/storage/innobase/handler/handler0alter.cc:3246-3415` builds added
  foreign-key metadata from `ALTER TABLE`, verifies child and parent indexes,
  validates FK options, and records accepted FK definitions for native
  InnoDB metadata.
- `mariadb/storage/innobase/dict/dict0mem.cc:959-988` tracks virtual-column
  dependencies for foreign keys.
- `mariadb/storage/innobase/row/row0ins.cc:901-935` recomputes affected virtual
  generated-column values during cascading actions when FK metadata records
  such dependencies.
- `packages/libmylite/src/database.cc:2939-3007` brackets direct ownerless SQL
  with dictionary begin/finish handling.
- `packages/libmylite/src/database.cc:9299-9308` runs the unsafe
  `dictionary-before-finish` hook after native SQL succeeds and before MyLite
  marks ownerless dictionary DDL complete.

## Design

Add a hook-only selector,
`dictionary-generated-column-foreign-key-crash`, to
`mylite_ownerless_cross_process_sql_test`.

The selector initializes one ownerless database with parent/child tables for
two representative accepted shapes:

1. A child table with a stored generated `parent_key` column and a supporting
   index. A killed writer adds a foreign key from the generated child column to
   a regular parent primary key.
2. A parent table with a unique stored generated `parent_key` column and a
   child table with a regular FK column. A killed writer adds a foreign key
   from the regular child column to the stored generated referenced column.

Each writer runs with a live ownerless peer and is killed at
`dictionary-before-finish`. Cleanup must remain busy until the live peer is
released, then no-live recovery must expose the completed native metadata.

After recovery, verify:

- `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS` and `KEY_COLUMN_USAGE` metadata,
- generated-column values,
- missing-parent insert errors,
- restricted parent-key update errors,
- `ON DELETE CASCADE` behavior,
- valid inserts after the cascade boundary,
- ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` recovery for a generated child-column FK,
- crash-at-`dictionary-before-finish` recovery for a generated referenced-column FK,
- live-peer cleanup-busy behavior,
- generated values and referential enforcement after no-live recovery,
- ownerless/native reopen before and after forced shared-memory rebuild.

Out of scope:

- MariaDB-rejected generated-column action clauses,
- virtual generated child-column FK creation beyond existing policy coverage,
- generated-column FK drop crash recovery,
- long-running randomized FK graph or RQG stress,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No new SQL feature is enabled. The slice strengthens the ownerless
generated-column and foreign-key claims by proving representative successful
generated-column FK DDL survives a writer crash at MyLite's dictionary
publication boundary.

The coverage remains partial: rejected generated-column FK options, FK drop
crashes, and randomized external FK stress remain planned.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector exercises existing
ownerless process-slot cleanup, dictionary-generation recovery, `.shm` rebuild,
and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

No InnoDB format change. MyLite continues to rely on MariaDB/InnoDB native
generated-column and foreign-key metadata; this slice proves ownerless
coordination can recover around completed native metadata changes.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-generated-column-foreign-key-crash`.
- Run adjacent hook coverage:
  `dictionary-foreign-key-crash` and
  `dictionary-generated-column-success-crash`.
- Run adjacent non-hook generated/FK selectors:
  `generated-column-foreign-key`,
  `generated-column-foreign-key-policy`, and `foreign-key-ddl`.
- Run registered ownerless hook CTest, embedded ownerless SQL shards,
  ownerless stress, `format-check`, `tidy`, and `git diff --check`.

## Acceptance Criteria

- Each killed writer reaches `dictionary-before-finish` without hanging.
- A live ownerless peer prevents cleanup until no-live recovery.
- Recovered generated child-column FK metadata is present and enforced.
- Recovered generated referenced-column FK metadata is present and enforced.
- Generated values remain correct before and after cascaded deletes and valid
  post-cascade inserts.
- Ownerless and ordinary native reopen observe the same final state before and
  after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic crash coverage for two accepted FK shapes, not an
  exhaustive generated-column FK matrix.
- Generated-column FK drop crash recovery remains separate recovery work.
- Full external MariaDB/RQG FK stress remains environment-owned follow-up work.
