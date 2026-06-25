# Ownerless Cross-Schema Multi-Rename Crash

## Problem

Ownerless hook coverage already kills single-table cross-schema rename writers
and same-schema multi-pair rename writers after MariaDB completes native DDL but
before MyLite publishes the ownerless dictionary finish. The remaining
DDL/file-lifecycle recovery gap still lacked the combined shape: one
multi-pair `RENAME TABLE` statement that moves file-per-table InnoDB tables
across schema directories.

This slice originally proved that the no-live recovery path reconstructs the
final cross-schema swapped state from native storage after the writer dies at
`dictionary-before-finish`. The follow-up
`docs/specs/ownerless-live-rename-list-recovery/specs.md` now upgrades this
selector to prove live-peer recovery for the same explicit schema-qualified
rename-list shape.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` executes the complete old/new rename list under
    table-name locks and delegates normal-table pairs through `rename_tables()`.
  - `rename_tables()` walks each old/new pair in one `RENAME TABLE` statement.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` builds schema-qualified old/new filenames, calls the
    storage-engine rename path, and renames the `.frm` metadata file.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` executes the native InnoDB DDL transaction
    and updates the InnoDB dictionary/table statistics metadata.
- `packages/libmylite/src/database.cc`
  - `ownerless_begin_dictionary_ddl()` arms ownerless dictionary coordination
    before DDL execution.
  - `ownerless_finish_dictionary_ddl()` exposes the unsafe
    `dictionary-before-finish` hook after native DDL success and before the
    ownerless dictionary generation is published.

## Scope And Non-Goals

In scope:

- Hook-build SQL coverage for a three-pair cross-schema InnoDB rename swap.
- Killing the writer at `dictionary-before-finish`.
- Live ownerless recovery while another peer still exists, with native file-op
  marker retention until final no-live drain.
- Ownerless recovery of final SQL metadata, native `.frm`/`.ibd` files, row
  contents, and swapped InnoDB `SPACE` identities.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Crash injection inside MariaDB's native rename loop.
- Rename rollback after a later pair fails.
- Foreign-key multi-rename crash behavior.
- A complete durable DDL file-lifecycle protocol.
- SQL-level table-lock wait fault injection; previous SQL shapes stopped
  before the ownerless table-wait callback.

## Design

Add a hook-only `dictionary-cross-schema-multi-rename-crash` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector creates `app.ownerless_cross_multi_rename_crash_left` and
`ownerless_cross_multi_rename_crash_archive.ownerless_cross_multi_rename_crash_right`,
records both InnoDB `SPACE` values, holds a live ownerless peer, and kills a
writer after this DDL reaches `dictionary-before-finish`:

```sql
RENAME TABLE
  app.ownerless_cross_multi_rename_crash_left
    TO ownerless_cross_multi_rename_crash_archive.ownerless_cross_multi_rename_crash_tmp,
  ownerless_cross_multi_rename_crash_archive.ownerless_cross_multi_rename_crash_right
    TO app.ownerless_cross_multi_rename_crash_left,
  ownerless_cross_multi_rename_crash_archive.ownerless_cross_multi_rename_crash_tmp
    TO ownerless_cross_multi_rename_crash_archive.ownerless_cross_multi_rename_crash_right
```

While the original peer remains open, a fresh ownerless opener now finishes the
dead dictionary generation and verifies the final names, absence of the
temporary name, swapped `SPACE` identities, row contents under the swapped
names, and post-recovery writes. The test then verifies the native file-op
marker remains set until the original peer exits, drains the marker with a
final no-live ownerless reopen, and checks ownerless/native reopen plus forced
`.shm` rebuild.

No production code change is intended unless the selector exposes a recovery
failure.

## Compatibility Impact

No SQL syntax or public C API behavior changes. The compatibility evidence for
ownerless hook-build DDL crash recovery expands from single-table cross-schema
rename and same-schema multi-pair rename to live recovery for the combined
cross-schema multi-pair rename shape. Overall DDL/file-lifecycle recovery
remains partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector verifies durable files remain inside
the MyLite database directory under `datadir/app/` and
`datadir/ownerless_cross_multi_rename_crash_archive/`, and that forced
`concurrency/mylite-concurrency.shm` rebuild preserves the native final state.

## Native Storage Impact

No storage-format changes. MariaDB's native InnoDB dictionary and
file-per-table rename state remain the recovery authority after the ownerless
dictionary finish is interrupted.

## Embedded Lifecycle And API

No public API changes. The slice covers embedded ownerless open/close behavior
around a killed DDL writer, live peer recovery, final no-live marker drain,
forced `.shm` rebuild, and ordinary native exclusive reopen.

## Build, Size, And Dependencies

No production binary-size, dependency, or license impact. The change adds
hook-build test code, a CTest entry, and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused `dictionary-cross-schema-multi-rename-crash`.
- Run adjacent dictionary rename crash selectors and the hook crash tail.
- Run product cross-schema/multi-rename replay selectors.
- Run production CTest subsets, format check, and `git diff --check`.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after the cross-schema
  three-pair rename can be recovered by a live ownerless opener while another
  peer remains open.
- After live recovery, the final left/right names exist, the temporary name
  is absent from SQL and InnoDB metadata, and final native files match the
  final schema directories.
- InnoDB `SPACE` identities are swapped relative to the pre-crash source names.
- Both swapped tables preserve rows and accept post-recovery writes.
- The native file-op marker remains set while the original live peer remains
  open, then final no-live ownerless recovery drains it.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same final state.

## Risks And Open Questions

- This proves the post-native-DDL, pre-ownerless-finish boundary. It does not
  prove crash handling inside the native rename loop.
- Broader DDL/file-lifecycle recovery, redo/checkpoint reconciliation, and
  randomized external DDL oracle stress remain planned.
