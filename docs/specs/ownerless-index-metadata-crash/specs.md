# Ownerless Index Metadata Crash

## Problem Statement

Ownerless normal-path coverage already proves `ALTER TABLE ... RENAME INDEX`
and `ALTER TABLE ... ALTER INDEX ... IGNORED` / `NOT IGNORED` refresh
already-open peers and survive reopen. Hook-build crash coverage, however,
stops at standalone secondary-index create/drop. The remaining gap is a killed
writer after MariaDB has applied metadata-only secondary-index ALTER state but
before MyLite publishes the ownerless dictionary generation.

This slice covers secondary-index rename and index ignorability crash
boundaries using the existing `dictionary-before-finish` unsafe test hook.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... ALTER INDEX index_name IGNORED` and `NOT IGNORED` into
  `Alter_index_ignorability`, and parses
  `ALTER TABLE ... RENAME INDEX old_name TO new_name` into
  `Alter_rename_key`, marking `ALTER_INDEX_IGNORABILITY` or
  `ALTER_RENAME_INDEX`.
- `mariadb/sql/sql_class.h` stores the requested index ignorability state in
  `Alter_index_ignorability::is_ignored()`.
- `mariadb/sql/sql_table.cc` filters missing `IF EXISTS` index operations,
  detects rename pairs by matching old and new index definitions whose only
  difference is name, sets `ALTER_RENAME_INDEX`, and maps
  `Alter_index_ignorability` entries by setting `KEY::is_ignored` on the new
  key metadata.
- `mariadb/sql/table.cc` restores `KEY::is_ignored` from `.frm` extra index
  flags and uses `TABLE_SHARE::set_ignored_indexes()` so ignored indexes are
  removed from the optimizer's usable-index bitmap.
- `mariadb/storage/innobase/handler/handler0alter.cc` allows the rename-index
  name-clash cases and commits in-place index metadata changes after native
  alter completion.
- MyLite `packages/libmylite/src/database.cc` begins the ownerless dictionary
  DDL state before executing MariaDB DDL and pauses at
  `dictionary-before-finish` immediately before publishing the stable
  generation.

## Design

Add focused unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The rename selector:

- creates an InnoDB table with rows and
  `ownerless_index_rename_crash_old_idx`,
- holds a live ownerless peer open,
- executes
  `ALTER TABLE app.ownerless_index_rename_crash_base RENAME INDEX
  ownerless_index_rename_crash_old_idx TO
  ownerless_index_rename_crash_new_idx` under the
  `dictionary-before-finish` fault,
- kills the writer at the hook,
- proves a new ownerless opener stays busy while the peer is live,
- releases the peer and verifies recovered metadata: old index absent, new
  index present, old `FORCE INDEX` rejected, new `FORCE INDEX` usable,
  subsequent DML accepted,
- verifies the final state through ownerless reopen, ordinary native reopen,
  forced `.shm` rebuild, and native reopen after rebuild.

The ignorability selector:

- creates an InnoDB table with rows and a secondary index,
- kills a writer after `ALTER INDEX ... IGNORED` but before dictionary finish,
- verifies the recovered `information_schema.statistics.IGNORED = 'YES'`
  state and later DML while the index is ignored,
- kills a second writer after `ALTER INDEX ... NOT IGNORED` but before
  dictionary finish,
- verifies the recovered `IGNORED = 'NO'` state, final `FORCE INDEX` reads,
  and ownerless/native reopen before and after forced `.shm` rebuild.

Both selectors reuse a small test helper for the live-peer/killed-writer/busy
probe sequence so the assertions stay focused on index metadata.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` coverage for completed secondary-index
  rename and ignorability metadata ALTERs,
- live-peer cleanup-busy behavior,
- no-live recovery plus ownerless/native reopen of final metadata.

Out of scope:

- new MyLite runtime behavior,
- randomized DDL crash exploration,
- optimizer plan equivalence while an index is ignored,
- special indexes, partitions, tablespace detach/import, or directory-option
  DDL classes that ownerless mode rejects or still marks planned,
- SQL-level table-lock fault injection.

## Compatibility Impact

No new SQL surface is enabled. This strengthens the evidence behind existing
partial ownerless `ALTER TABLE` support for MariaDB secondary-index metadata
operations.

## DDL Metadata Routing Impact

The covered statements use MariaDB's native ALTER TABLE metadata pipeline. The
slice only proves that MyLite's ownerless dictionary-generation boundary can be
recovered after the native metadata has changed and before MyLite publishes the
stable generation.

## Directory And Lifecycle Impact

No directory layout changes. Durable state remains in the MyLite-owned
database directory. The tests exercise existing ownerless `.shm` rebuild,
process-slot cleanup, dictionary-generation recovery, and ordinary native
exclusive reopen.

## Native Storage Impact

Native InnoDB table and secondary-index formats are unchanged. MyLite does not
rewrite the index metadata; it coordinates and verifies recovery around the
completed native ALTER.

## Public API, Wire Protocol, Build, Size, License, And Dependencies

No public API, wire-protocol, build-profile, binary-size, license, or
dependency changes.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `dictionary-secondary-index-rename-crash`
  - `dictionary-secondary-index-ignorability-crash`
- Run adjacent hook selectors:
  - `dictionary-secondary-index-crash`
  - `dictionary-secondary-index-drop-crash`
- Run normal selectors:
  - `rename-index-ddl`
  - `ignored-index-ddl`
- Run `crash-tail`.
- Run relevant embedded ownerless SQL, hook negative-proof, ownerless stress,
  `format-check`, `tidy`, and diff whitespace checks.

## Acceptance Criteria

- Both focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents crashed-writer cleanup until no-live recovery.
- Recovered rename metadata has the old index absent and new index usable.
- Recovered ignored metadata reports `IGNORED = 'YES'` and accepts DML.
- Recovered not-ignored metadata reports `IGNORED = 'NO'` and supports final
  `FORCE INDEX` reads.
- Ownerless and ordinary native reopen observe the same final metadata before
  and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- Deterministic hook coverage does not replace randomized external DDL oracles.
- The ignored-index check verifies MariaDB's metadata and final forced-index
  usability, not detailed optimizer plan choice while the index is ignored.
- SQL-level table-lock fault injection remains planned because previously
  explored SQL shapes stop before MyLite's ownerless table-wait callback.
