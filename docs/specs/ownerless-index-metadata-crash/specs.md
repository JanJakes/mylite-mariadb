# Ownerless Index Metadata Crash

## Problem Statement

Ownerless normal-path coverage already proves `CREATE OR REPLACE UNIQUE INDEX`,
`ALTER TABLE ... RENAME INDEX`, and `ALTER TABLE ... ALTER INDEX ... IGNORED` /
`NOT IGNORED` refresh already-open peers and survive reopen. Hook-build crash
coverage originally stopped at no-live recovery for standalone secondary-index
create/drop plus rename and ignorability. The remaining gap in this slice was a
killed writer after MariaDB had applied replacement unique-index metadata but
before MyLite published the ownerless dictionary generation.

This slice covers secondary-index rename, index ignorability, and unique-index
replacement crash boundaries using the existing `dictionary-before-finish`
unsafe test hook. The follow-up
`docs/specs/ownerless-index-metadata-live-recovery/specs.md` promotes those
boundaries, plus ordinary secondary-index create/drop and unique-index drop, to
live-peer recovery.

Completed unique secondary-index drop crash recovery is covered separately by
`ownerless-unique-index-drop-ddl-crash`.
Online `ALTER TABLE ... ADD/DROP INDEX ..., ALGORITHM=INPLACE, LOCK=NONE`
crash recovery is covered separately by
`docs/specs/ownerless-online-index-ddl-crash-recovery/specs.md`.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses
  `CREATE OR REPLACE UNIQUE INDEX` into `Key::UNIQUE` with the `OR REPLACE`
  option, parses
  `ALTER TABLE ... ALTER INDEX index_name IGNORED` and `NOT IGNORED` into
  `Alter_index_ignorability`, and parses
  `ALTER TABLE ... RENAME INDEX old_name TO new_name` into
  `Alter_rename_key`, marking `ALTER_INDEX_IGNORABILITY` or
  `ALTER_RENAME_INDEX`.
- `mariadb/sql/sql_class.h` stores the requested index ignorability state in
  `Alter_index_ignorability::is_ignored()`.
- `mariadb/sql/sql_table.cc` filters missing `IF EXISTS` index operations,
  handles `key->or_replace()` by adding the existing key to the alter drop
  list before adding the replacement key,
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
- recovers through a new ownerless opener while the peer is live and retains
  the prearmed native file-operation marker until the last live peer exits,
- verifies recovered metadata: old index absent, new
  index present, old `FORCE INDEX` rejected, new `FORCE INDEX` usable,
  subsequent DML accepted,
- releases the peer,
- verifies the final state through ownerless reopen, ordinary native reopen,
  forced `.shm` rebuild, and native reopen after rebuild.

The unique replacement selector:

- creates an InnoDB table and a unique index over `(tenant_id, slug)`,
- verifies duplicate old-key writes fail,
- kills a writer after
  `CREATE OR REPLACE UNIQUE INDEX ... (tenant_id, weight)` but before
  dictionary finish,
- recovers through a new ownerless opener while the peer is live and keeps the
  native file-operation marker set until no-live drain,
- verifies recovered metadata has the same index name over `weight`, no longer
  includes `slug`, accepts the formerly duplicate `(tenant_id, slug)` shape,
  and rejects duplicate `(tenant_id, weight)` writes,
- verifies ownerless/native reopen before and after forced `.shm` rebuild.

The ignorability selector:

- creates an InnoDB table with rows and a secondary index,
- kills a writer after `ALTER INDEX ... IGNORED` but before dictionary finish,
- recovers through a new ownerless opener while the peer is live and retains
  the prearmed native file-operation marker until the last live peer exits,
- verifies the recovered `information_schema.statistics.IGNORED = 'YES'`
  state and later DML while the index is ignored,
- kills a second writer after `ALTER INDEX ... NOT IGNORED` but before
  dictionary finish,
- verifies the recovered `IGNORED = 'NO'` state, final `FORCE INDEX` reads,
  and ownerless/native reopen before and after forced `.shm` rebuild.

The promoted selectors reuse the held-live-peer/killed-writer helper so the
assertions stay focused on index metadata and marker policy.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` coverage for completed secondary-index
  unique replacement, rename, and ignorability metadata ALTERs,
- live-peer recovery behavior,
- marker-retaining physical unique replacement and prearmed-marker
  rename/ignorability recovery,
- ownerless/native reopen of final metadata.

Covered separately:

- completed native drop of an active unique secondary index, including
  duplicate-key enforcement release after recovery
  (`ownerless-unique-index-drop-ddl-crash`).

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

The covered statements use MariaDB's native ALTER TABLE metadata pipeline.
Even a metadata-only ALTER can commit native dictionary redo before MyLite
publishes its stable dictionary generation. MyLite therefore keeps the
conservative marker that was armed before the statement until the ownerless
dictionary finish becomes durable. A crash before that finish leaves the
marker available to force native recovery; a successful finish clears a marker
that the statement introduced when no physical file operation independently
requires it.

## Directory And Lifecycle Impact

No directory layout changes. Durable state remains in the MyLite-owned
database directory. The tests exercise existing ownerless `.shm` rebuild,
process-slot cleanup, live-peer dictionary-generation recovery, final no-live
marker drain where physical index work occurred, and ordinary native exclusive
reopen.

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
  - `dictionary-unique-index-replace-crash`
- Run normal selectors:
  - `rename-index-ddl`
  - `ignored-index-ddl`
- Run `crash-tail`.
- Run relevant embedded ownerless SQL, hook negative-proof, ownerless stress,
  `format-check`, `tidy`, and diff whitespace checks.

## Acceptance Criteria

- Focused selectors reach the dictionary fault hook and do not hang.
- Recovery succeeds while another ownerless peer remains live.
- Physical unique replacement and crashed prearmed metadata changes retain the
  native file-operation marker until final no-live recovery. A normally
  completed metadata-only rename or ignored/not-ignored change clears its
  statement-local prearm after the durable ownerless dictionary finish.
- Recovered rename metadata has the old index absent and new index usable.
- Recovered unique replacement metadata has the replacement key part and
  enforces replacement-key duplicates while old-key duplicates are allowed.
- Recovered unique-index drop behavior is covered by
  `ownerless-unique-index-drop-ddl-crash`.
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
