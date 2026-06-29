# Ownerless FK Index Mixed Alter Live Recovery

## Problem

Ownerless live-peer dictionary recovery already accepts bounded mixed
`ALTER TABLE` lists that combine foreign-key add/drop clauses with selected
non-FK table-definition clauses. Secondary-index ADD/DROP clauses were still
accepted only as standalone ALTER statements. A writer death after MariaDB
completed one native statement that drops an FK, drops a secondary index, adds
a secondary index, and adds another FK could therefore leave a live peer unable
to classify the completed statement as recoverable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` `alter_list_item` parses `ALTER TABLE` ADD
  index clauses and DROP index clauses, setting `ALTER_ADD_INDEX` and
  `ALTER_DROP_INDEX` flags.
- `mariadb/sql/sql_table.cc` `mysql_prepare_alter_table()` and
  `mysql_alter_table()` resolve mixed ALTER lists, split add/drop index
  buffers, and also track `ALTER_ADD_FOREIGN_KEY` and
  `ALTER_DROP_FOREIGN_KEY` for the same statement.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `ha_innobase::check_if_supported_inplace_alter()` examines
  `index_add_count`, `index_drop_count`, `ALTER_ADD_FOREIGN_KEY`, and
  `ALTER_DROP_FOREIGN_KEY`; later InnoDB alter paths consume index add/drop
  buffers and FK changes.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_add_index_recovery_statement()` and
  `ownerless_alter_table_drop_index_recovery_statement()` already provide
  bounded single-clause metadata checks for named secondary index ADD/DROP.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_mixed_foreign_key_recovery_statement()` already
  classifies bounded FK add/drop mixed lists and excludes generated-column FK
  tables.

## Design

Extend the mixed foreign-key ALTER-list parser with two bounded secondary-index
clause consumers:

- `ADD [UNIQUE] INDEX|KEY name (column [ASC|DESC], ...)`
- `DROP INDEX|KEY name`

The mixed list must still include at least one FK add and one FK drop. Index
clauses are accepted only when pre-execution metadata proves the added index is
absent, the dropped index exists, and the named key columns exist. Idempotent
`IF [NOT] EXISTS`, unnamed indexes, prefix lengths, FULLTEXT/SPATIAL indexes,
and combinations with column mutation clauses remain outside this slice. The
selector proves the focused FK plus index shape; additional metadata-only
clauses are not claimed beyond the existing focused metadata slices.

The implementation reuses existing recovery kinds instead of adding a new
dictionary state value. Mixed index-only non-FK clauses choose the index
file-operation lane: `CREATE_INDEX` for ADD INDEX or combined ADD/DROP INDEX,
and `DROP_INDEX` when only DROP INDEX appears. Both lanes already force the
native file-operation checkpoint marker while a peer remains live and drain it
during final no-live recovery.

## Compatibility Impact

No public API change. The accepted SQL shape follows MariaDB syntax for named
secondary-index clauses and foreign-key clauses. After ownerless recovery,
MariaDB metadata and enforcement must show:

- the dropped FK absent,
- the surviving and added FKs enforced,
- the dropped secondary index absent,
- the added unique secondary index visible through `INFORMATION_SCHEMA` and
  usable by `FORCE INDEX`.

## Directory And Native Storage Impact

No new durable files or formats are introduced. The ALTER statement can perform
native secondary-index file operations, so MyLite treats this focused mixed
shape like other real index ADD/DROP recovery: the native file-operation marker
remains durable while a live peer exists, then clears only after final no-live
recovery checkpoints native state.

## Test Plan

- Add hook-build selector:
  `dictionary-foreign-key-mixed-index-alter-crash`.
- The selector creates parent A/B/C tables and a child table with FK A, FK B,
  FK helper indexes, and an old secondary value index.
- Kill the writer at `dictionary-before-finish` after executing a single
  `ALTER TABLE` that drops FK A, drops the old value index, adds a unique value
  index, and adds FK C.
- Verify a live ownerless peer sees FK A removed, FK B/FK C enforced, the old
  value index absent, the new unique value index visible and usable, and the
  native file-operation marker retained.
- Verify final no-live recovery drains the marker, then ownerless/native reopen
  and forced `.shm` rebuild preserve the state.

## Acceptance Criteria

- Mixed FK plus secondary-index ADD/DROP ALTER lists are accepted only through
  the bounded parser and retain existing generated-column FK exclusions.
- The focused crash selector passes in the unsafe hook preset.
- The native file-operation marker remains set while a live peer exists and is
  cleared by no-live recovery.
- Docs distinguish this focused mixed index case from broader arbitrary mixed
  ALTER support.

## Risks And Follow-Up

- Broader mixed FK/non-FK ALTER lists remain planned beyond the covered
  ADD/DROP column, MODIFY/CHANGE/RENAME column, table-comment, column-default,
  named CHECK, and named secondary-index cases.
- Standalone idempotent index recovery is already covered separately; this
  slice intentionally does not mix those no-op forms into FK lists.
- Longer randomized external MariaDB DDL stress is still needed before claiming
  arbitrary mixed ALTER coverage.
