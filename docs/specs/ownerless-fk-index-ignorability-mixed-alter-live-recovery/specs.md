# Ownerless FK Index Ignorability Mixed Alter Live Recovery

## Problem

Ownerless live-peer dictionary recovery already accepts standalone
`ALTER TABLE ... ALTER INDEX ... IGNORED|NOT IGNORED` as a metadata-only
secondary-index change. Mixed foreign-key ALTER-list parsing did not consume
that clause, so a writer death after MariaDB completed one statement that drops
an FK, changes secondary-index ignorability, and adds another FK could leave a
live peer unable to classify the completed statement as recoverable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` `alter_list_item` parses
  `ALTER key_or_index ... ignorability` and sets `ALTER_INDEX_IGNORABILITY`.
- `mariadb/sql/sql_table.cc` `fill_alter_inplace_info()` applies the
  requested `is_ignored` value to the new key metadata and records altered
  index ignorability.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `INNOBASE_INPLACE_IGNORE` includes `ALTER_INDEX_IGNORABILITY`, so InnoDB
  treats the operation as metadata-only for inplace support.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_alter_index_ignorability_recovery_statement()` already
  provides bounded single-clause metadata checks for an existing index.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_mixed_foreign_key_recovery_statement()` already
  classifies bounded FK add/drop mixed lists and excludes generated-column FK
  tables.

## Design

Add a clause-level consumer for:

- `ALTER INDEX name IGNORED`
- `ALTER INDEX name NOT IGNORED`

The mixed list must still include at least one FK drop and one FK add. The
alter-index clause is accepted only when pre-execution metadata proves the
named index exists. It is accepted only when no other non-FK recovery kind has
already been selected in this bounded parser path. This slice proves the
`IGNORED` direction; the clause consumer accepts `NOT IGNORED` because the
standalone recovery path already supports both spellings through the same
metadata validation.

The implementation reuses
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_INDEX_IGNORABILITY`, which is
metadata-only in live-peer dead-owner recovery. Standalone index ignorability
retains its prearmed native-dictionary marker across a crash, while the focused
mixed FK plus index-ignorability ALTER also emits native file-operation
evidence in the hook test. The live-peer recovery path must therefore recover
through the ignorability dictionary lane while
retaining the native file-operation checkpoint marker until final no-live
drain.

## Compatibility Impact

No public API change. The accepted SQL shape follows MariaDB syntax for named
secondary-index ignorability clauses and foreign-key clauses. After ownerless
recovery, MariaDB metadata and enforcement must show:

- the dropped FK absent,
- the surviving and added FKs enforced,
- the secondary index still present with `INFORMATION_SCHEMA.STATISTICS.ignored`
  set to `YES`.

## Directory And Native Storage Impact

No new durable files or formats are introduced. The combined FK plus
secondary-index ignorability ALTER can emit native file-operation redo, so
MyLite retains the native file-operation checkpoint marker while a live peer
exists and clears it only after final no-live recovery drains native state.

## Test Plan

- Add hook-build selector:
  `dictionary-foreign-key-mixed-ignored-index-alter-crash`.
- The selector creates parent A/B/C tables and a child table with FK A, FK B,
  FK helper indexes, and a secondary value index.
- Kill the writer at `dictionary-before-finish` after executing a single
  `ALTER TABLE` that drops FK A, marks the value index ignored, and adds FK C.
- Verify a live ownerless peer sees FK A removed, FK B/FK C enforced, the value
  index metadata set to `ignored = 'YES'`, and the native file-operation marker
  retained.
- Verify final no-live recovery drains the marker, then ownerless/native reopen
  and forced `.shm` rebuild preserve the state.

## Acceptance Criteria

- Mixed FK plus secondary-index `ALTER INDEX ... IGNORED|NOT IGNORED` ALTER
  lists are accepted only through the bounded parser and retain existing
  generated-column FK exclusions.
- The focused crash selector passes in the unsafe hook preset.
- The native file-operation marker remains set while a live peer exists and is
  cleared by no-live recovery.
- Docs distinguish this focused mixed ignorability case from broader arbitrary
  mixed ALTER support.

## Risks And Follow-Up

- A dedicated mixed `NOT IGNORED` crash selector remains a possible follow-up
  if future evidence needs both directions in mixed FK lists.
- Combining index ignorability with secondary-index ADD/DROP, secondary-index
  rename, or column mutation clauses remains intentionally unsupported until
  tested as its own slice.
- Longer randomized external MariaDB DDL stress is still needed before claiming
  arbitrary mixed ALTER coverage.
