# Ownerless FK Index Rename Mixed Alter Live Recovery

## Problem

Ownerless live-peer dictionary recovery accepts bounded mixed `ALTER TABLE`
lists that combine foreign-key add/drop clauses with selected non-FK metadata
and file-operation clauses. Standalone secondary-index rename recovery already
uses a metadata-only recovery lane, but mixed foreign-key ALTER-list parsing
treated `RENAME` as column rename only. A writer death after MariaDB completed
one statement that drops an FK, renames a secondary index, and adds another FK
therefore could leave a live peer unable to classify the completed statement as
recoverable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` `alter_list_item` parses
  `RENAME key_or_index ... TO ...` as an `ALTER TABLE` list item and sets
  `ALTER_RENAME_INDEX`.
- `mariadb/sql/sql_table.cc` `fill_alter_inplace_info()` detects equivalent
  add/drop key pairs as index renames, records `rename_keys`, and also handles
  explicit ALTER index metadata changes in the same ALTER preparation flow.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `ha_innobase::check_if_supported_inplace_alter()` validates renamed index
  metadata through `ALTER_RENAME_INDEX`, and later InnoDB alter completion
  updates index cache names through `innobase_rename_indexes_cache()`.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_rename_index_recovery_statement()` already provides
  a bounded single-clause metadata check for `ALTER TABLE ... RENAME INDEX`.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_mixed_foreign_key_recovery_statement()` already
  classifies bounded FK add/drop mixed lists and excludes generated-column FK
  tables.

## Design

Add a clause-level consumer for:

- `RENAME INDEX|KEY old_name TO new_name`

The mixed list must still include at least one FK drop and one FK add. The
index rename clause is accepted only when pre-execution metadata proves the old
index exists and the new index name does not exist. The rename-index clause is
accepted only as the single non-FK recovery lane in this slice; combinations
with secondary-index ADD/DROP, column mutation, or other file-operation clauses
remain outside this bounded parser path unless separately tested.

The implementation reuses
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_INDEX`. Standalone
secondary-index rename is metadata-only, but the focused mixed FK plus
rename-index ALTER emits native file-operation evidence in the hook test. The
live-peer recovery path must therefore recover through the rename-index
dictionary lane while retaining the native file-operation checkpoint marker
until final no-live drain.

## Compatibility Impact

No public API change. The accepted SQL shape follows MariaDB syntax for named
secondary-index rename clauses and foreign-key clauses. After ownerless
recovery, MariaDB metadata and enforcement must show:

- the dropped FK absent,
- the surviving and added FKs enforced,
- the old secondary-index name absent,
- the renamed secondary-index name visible through `INFORMATION_SCHEMA` and
  usable by `FORCE INDEX`.

## Directory And Native Storage Impact

No new durable files or formats are introduced. The combined FK plus
secondary-index rename ALTER can emit native file-operation redo, so MyLite
retains the native file-operation checkpoint marker while a live peer exists
and clears it only after final no-live recovery drains native state.

## Test Plan

- Add hook-build selector:
  `dictionary-foreign-key-mixed-rename-index-alter-crash`.
- The selector creates parent A/B/C tables and a child table with FK A, FK B,
  FK helper indexes, and an old secondary value index.
- Kill the writer at `dictionary-before-finish` after executing a single
  `ALTER TABLE` that drops FK A, renames the old value index, and adds FK C.
- Verify a live ownerless peer sees FK A removed, FK B/FK C enforced, the old
  value index absent, the renamed value index visible and usable, and the
  native file-operation marker retained.
- Verify final no-live recovery drains the marker, then ownerless/native reopen
  and forced `.shm` rebuild preserve the state.

## Acceptance Criteria

- Mixed FK plus secondary-index `RENAME INDEX|KEY` ALTER lists are accepted
  only through the bounded parser and retain existing generated-column FK
  exclusions.
- The focused crash selector passes in the unsafe hook preset.
- The native file-operation marker remains set while a live peer exists and is
  cleared by no-live recovery.
- Docs distinguish this focused mixed rename-index case from broader arbitrary
  mixed ALTER support.

## Risks And Follow-Up

- `ALTER INDEX ... IGNORED|NOT IGNORED` mixed with FK add/drop remains a
  separate metadata-only follow-up.
- Combining secondary-index rename with ADD/DROP index clauses or column
  mutation clauses remains intentionally unsupported until tested as its own
  slice.
- Longer randomized external MariaDB DDL stress is still needed before
  claiming arbitrary mixed ALTER coverage.
