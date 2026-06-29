# Ownerless FK Primary-Key Mixed Alter Live Recovery

## Problem

Ownerless live-peer recovery covers standalone primary-key replacement and many
focused mixed foreign-key plus non-FK ALTER lists. The mixed parser still does
not accept a single ALTER statement that drops a foreign key, replaces the
child table primary key, and adds a new foreign key. That leaves a clustered
index rebuild class outside the focused mixed FK matrix.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` `alter_list_item` parses `DROP FOREIGN KEY`,
  `DROP PRIMARY KEY`, and `ADD key_def` as comma-separated ALTER-list entries.
- `mariadb/sql/sql_table.cc` `fill_alter_inplace_info()` maps dropped and
  added primary keys to `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects a bare InnoDB
  `DROP PRIMARY KEY` unless it is paired with an added primary key, and treats
  a new primary key as a clustered-index rebuild path.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_replace_primary_key_recovery_statement()` already
  provides a bounded primary-key replacement classifier and metadata proofs.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_mixed_foreign_key_recovery_statement()` already
  classifies bounded FK add/drop mixed lists and excludes generated-column FK
  tables.

## Design

Factor the existing primary-key replacement grammar into a clause-level
consumer for:

```sql
DROP PRIMARY KEY, ADD PRIMARY KEY (<key-part>[, ...])
```

The consumer keeps the standalone classifier's metadata requirements:

- pre-execution metadata proves the table has `PRIMARY`;
- every replacement key-part column exists;
- key parts remain bare column names with optional `ASC` or `DESC`;
- no prefix lengths, option tails, or alternate primary-key action order are
  accepted.

The mixed foreign-key parser accepts the primary-key replacement only when the
ALTER list also includes at least one FK drop and one FK add, and only when no
other non-FK recovery kind has been selected. It reuses
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_REPLACE_PRIMARY_KEY`, which
stays on the native file-operation checkpoint lane.

## Compatibility Impact

No public API changes. The accepted SQL shape follows MariaDB ALTER-list
syntax and strengthens ownerless crash recovery evidence for an existing
MariaDB-compatible clustered-index rebuild class.

After recovery, MariaDB metadata and enforcement must show:

- the dropped FK absent;
- the surviving and added FKs enforced;
- `PRIMARY` moved from the old key column to the replacement key column;
- duplicate replacement-key writes rejected and duplicate old-key writes
  accepted.

## Directory And Native Storage Impact

No new files or formats are introduced. The primary-key replacement is a native
InnoDB clustered-index rebuild, so MyLite must retain the native file-operation
checkpoint marker while a live peer exists and drain it only through final
no-live recovery.

## Test Plan

- Add hook-build selector:
  `dictionary-foreign-key-mixed-primary-key-alter-crash`.
- The selector creates parent A/B/C tables and a child table with
  `PRIMARY(id)`, unique `code`, FK A, and FK B.
- Kill the writer at `dictionary-before-finish` after executing one
  `ALTER TABLE` that drops FK A, drops `PRIMARY`, adds `PRIMARY(code)`, and
  adds FK C.
- Verify a live ownerless peer sees FK A removed, FK B/FK C enforced,
  `PRIMARY(code)` metadata and enforcement, and the native file-operation
  marker retained.
- Verify final no-live recovery drains the marker, then ownerless/native reopen
  and forced `.shm` rebuild preserve the state.

## Acceptance Criteria

- Mixed FK plus primary-key replacement ALTER lists are accepted only through
  the bounded parser and retain existing generated-column FK exclusions.
- The focused crash selector passes in the unsafe hook preset.
- The native file-operation marker remains set while a live peer exists and is
  cleared by no-live recovery.
- Docs distinguish this focused mixed clustered-index rebuild case from
  arbitrary mixed ALTER support.

## Risks And Follow-Up

- AUTO_INCREMENT, descending, composite, generated-column, and FK-parent
  primary-key mixed ALTER lists remain separate candidate slices.
- Combining primary-key replacement with other non-FK ALTER clauses remains
  intentionally unsupported until tested as its own slice.
- Longer randomized external MariaDB DDL stress is still needed before
  claiming arbitrary mixed ALTER coverage.
