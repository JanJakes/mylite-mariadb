# Ownerless Live Rename-List Recovery

## Problem Statement

Ownerless live dictionary recovery already covers the conservative
single-pair same-schema `RENAME TABLE schema.table TO schema.other_table`
shape. The remaining rename gap is broader: MariaDB can complete native
file-per-table movement for cross-schema rename, multi-pair swap cycles, and
foreign-key parent/child rename lists before MyLite publishes the ownerless
dictionary finish. A live peer should not force those completed native rename
states to wait for no-live recovery when MyLite has durable native file-op
evidence and a bounded statement classifier.

This slice upgrades explicit schema-qualified rename-list recovery so a fresh
ownerless opener can finish the dead dictionary generation while another
ownerless peer remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` rejects locked-table and active transaction
    contexts, locks the old/new table names, and delegates the ordered pair
    list to `rename_tables()`.
  - `rename_tables()` treats each two `TABLE_LIST` entries as one old/new
    pair, runs every pair in statement order, and reverts normal-table renames
    through the DDL log on error.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` builds schema-qualified old/new filenames, calls
    the handler rename path, and renames the `.frm` metadata file.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` runs the InnoDB DDL transaction, locks
    affected dictionary and FK metadata, delegates to `innobase_rename_table()`,
    and flushes redo for the successful DDL.
- `packages/libmylite/src/database.cc`
  - `ownerless_finish_dictionary_ddl()` writes the native file-op checkpoint
    marker before the `dictionary-before-finish` hook when InnoDB file-op redo
    was observed, then marks the dictionary owner recoverable when the
    statement classifier provides a recovery kind.
  - `ownerless_dictionary_recovery_kind_for_statement()` previously accepted
    only single-pair same-schema `RENAME TABLE`; this slice broadens it to
    explicit schema-qualified rename lists.

## Scope And Non-Goals

In scope:

- Explicit `RENAME TABLE schema.table TO schema.table [, ...]` lists with one
  or more schema-qualified old/new pairs.
- Same-schema, cross-schema, same-schema multi-pair swap, cross-schema
  multi-pair swap, same-schema foreign-key parent/child multi-rename, and
  cross-schema foreign-key parent/child multi-rename crash selectors.
- Live-peer dictionary recovery after a writer dies at
  `dictionary-before-finish`.
- Durable native file-op marker retention while the original live peer remains
  open and final marker drain after no live peers remain.

Out of scope:

- Temporary-table rename and malformed rename lists. `RENAME TABLE IF EXISTS`
  is covered separately by
  `docs/specs/ownerless-rename-if-exists-live-recovery/specs.md`.
  Implicit-schema rename is covered separately by
  `docs/specs/ownerless-implicit-rename-recovery/specs.md`, and view-only
  rename is covered separately by
  `docs/specs/ownerless-view-rename-live-recovery/specs.md`.
- Crash injection inside MariaDB's native rename loop or DDL-log rollback path.
- Foreign-key ADD/DROP live recovery outside rename-list file movement.
- Broader schema, view, trigger, partition, import/export, and arbitrary ALTER
  file-lifecycle recovery.
- SQL-level table-lock wait fault injection.

## Design

Rename recovery continues to use the existing
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE` marker. The dictionary
state layout and recovery-kind set do not change.

`ownerless_dictionary_recovery_kind_for_statement()` now accepts a raw-token
shape for explicit schema-qualified rename lists:

```sql
RENAME TABLE schema.table TO other_schema.other_table[, ...]
```

The classifier requires every old and new name to be represented as
`identifier . identifier`, requires `TO` between each old/new pair, allows
commas only between complete pairs, and allows only trailing semicolons after
the final pair. Because `ownerless_finish_dictionary_ddl()` marks the
dictionary owner recoverable only after native file-op redo has been observed,
the classifier alone does not make non-InnoDB table rename classes live
recoverable. View-only rename uses the separate metadata-only kind documented in
`docs/specs/ownerless-view-rename-live-recovery/specs.md`.

Hook tests now keep the original peer open after killing the writer, open a
second ownerless handle, verify the final renamed state and post-recovery
writes through that live handle, then confirm the native file-op marker remains
set until the original live peer exits. A final no-live ownerless reopen drains
the marker and the existing ownerless/native reopen checks verify durability
before and after forced `.shm` rebuild.

## Compatibility Impact

Successful SQL behavior is unchanged. Crash recovery evidence expands from
single-pair same-schema rename to explicit schema-qualified rename lists,
including cross-schema file movement, swap cycles through temporary names, and
foreign-key parent/child rename metadata. Overall DDL crash recovery remains
partial because many non-rename DDL classes still wait for no-live recovery or
lack live recovery coverage.

## Directory And Lifecycle Impact

No directory layout changes. The slice relies on MariaDB native `.frm` and
InnoDB `.ibd` movement inside the MyLite database directory, the existing
`concurrency/mylite-concurrency.ckpt` native file-op marker, and the existing
final no-live checkpoint drain.

## Native Storage Impact

No storage-format changes. MariaDB native InnoDB dictionary, file-per-table
identity, `SPACE` values, and FK metadata remain the source of truth after a
writer dies past native DDL success and before ownerless dictionary finish.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation changes first-party ownerless SQL classification
and hook-test coverage only.

## Test And Verification Plan

- Build hook targets for `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selectors:
  `dictionary-cross-schema-rename-crash`,
  `dictionary-multi-rename-crash`,
  `dictionary-cross-schema-multi-rename-crash`,
  `dictionary-foreign-key-multi-rename-crash`, and
  `dictionary-foreign-key-cross-schema-multi-rename-crash`.
- Run adjacent same-schema rename marker coverage.
- Run production ownerless SQL replay selectors for renamed tablespaces,
  cross-schema rename, multi-rename, and FK rename refresh where registered.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- A writer killed after native rename work but before ownerless dictionary
  finish no longer forces explicit schema-qualified rename-list cleanup to wait
  for the original live peer to exit.
- The live opener observes final table names, absent temporary names, preserved
  or swapped InnoDB `SPACE` identities, FK metadata where relevant, and
  successful post-recovery writes.
- The native file-op marker remains set while the original peer is live.
- After the original peer exits, final no-live ownerless recovery drains the
  marker and preserves the final state through ownerless/native reopen and
  forced `.shm` rebuild.

## Risks And Follow-Up

- The classifier is intentionally syntactic. It admits explicit
  schema-qualified native rename lists only when InnoDB file-op redo evidence
  exists, but it does not prove crashes inside MariaDB's rename loop.
- Temporary-table rename forms remain planned until they have focused recovery
  coverage. The `IF EXISTS` rename syntax is covered separately by
  `docs/specs/ownerless-rename-if-exists-live-recovery/specs.md`.
- Broader DDL/file-lifecycle recovery, transaction crash windows,
  active-reader pressure crash/oracle breadth, and randomized external
  MariaDB/RQG stress remain open ownerless-concurrency completion work.
