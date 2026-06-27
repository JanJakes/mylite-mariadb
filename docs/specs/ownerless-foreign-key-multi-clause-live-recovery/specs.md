# Ownerless Foreign-Key Multi-Clause Live Recovery

## Problem Statement

Ownerless live-peer dictionary recovery covers single-clause
`ALTER TABLE ... ADD [CONSTRAINT] FOREIGN KEY ... REFERENCES ...` and
single-clause `ALTER TABLE ... DROP FOREIGN KEY ...` for ordinary non-generated
InnoDB tables. The statement classifier deliberately rejects comma-separated
foreign-key ALTER clauses, so a successful native ALTER that adds or drops
multiple FK constraints can be killed after MariaDB finishes but before MyLite
publishes dictionary finish without the metadata-only recoverable ownerless
marker.

That leaves a documented non-rename FK multi-DDL recovery gap even though pure
FK add/drop lists use the same MariaDB/InnoDB metadata machinery as the covered
single-clause forms.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:6153-6165` parses table-level
  `FOREIGN KEY` definitions and records them in the ALTER key list.
- `mariadb/sql/sql_yacc.yy:8051` parses `DROP FOREIGN KEY` table elements.
- `mariadb/sql/sql_table.cc:6578-6675` handles
  `ALTER TABLE ADD FOREIGN KEY IF NOT EXISTS`/duplicate handling and notes that
  an ADD FOREIGN KEY appends paired key-list entries.
- `mariadb/sql/sql_table.cc:9586-9592` keeps `DROP FOREIGN KEY` names in the
  ALTER drop list and compares them against existing child FK metadata.
- `mariadb/storage/innobase/handler/handler0alter.cc:123-126` classifies
  InnoDB FK add/drop as schema-only alter operations.
- `mariadb/storage/innobase/handler/handler0alter.cc:10046-10133` updates
  InnoDB FK definitions in dictionary tables/caches during ALTER completion.
- Before this slice, `packages/libmylite/src/database.cc` marked single ordinary FK
  add/drop recovery as metadata-only and skips native file-op checkpoint
  markers for those two recovery kinds.
- Before this slice, `packages/libmylite/src/database.cc` rejected commas inside
  `ownerless_alter_table_add_foreign_key_recovery_statement()` and only accepts
  one `DROP FOREIGN KEY` clause in
  `ownerless_alter_table_drop_foreign_key_recovery_statement()`.

## Scope And Non-Goals

In scope:

- Accept pure comma-separated FK add lists:
  `ALTER TABLE child ADD [CONSTRAINT name] FOREIGN KEY (...) REFERENCES parent (...), ADD ...`.
- Accept pure comma-separated FK drop lists:
  `ALTER TABLE child DROP FOREIGN KEY fk1, DROP FOREIGN KEY fk2`.
- Keep the existing generated-column guard: child and referenced tables with
  generated columns remain outside the metadata-only FK live-recovery lane.
- Add hook crash coverage for one two-FK add list and one two-FK drop list,
  killed at `dictionary-before-finish` while a live ownerless peer remains open.
- Verify recovered metadata, enforcement or absence, native marker clear,
  ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Mixed ALTER lists that combine FK clauses with column, index, CHECK, rebuild,
  table option, or rename clauses.
- Generated-column FK metadata-only live recovery.
- MariaDB internal crash points before native ALTER completion.
- SQL-level table-lock fault injection; prior explored SQL shapes did not
  reach the ownerless table-wait callback.
- External MariaDB/RQG stress expansion.

## Design

Refactor the ownerless FK recovery statement recognizers into small clause
consumers:

- parse the `ALTER TABLE` target once and prove the child table is ordinary
  non-generated;
- consume one or more `ADD [CONSTRAINT name] FOREIGN KEY (...) REFERENCES
  table (...)` clauses separated only by commas, rejecting any other comma
  clause;
- consume one or more `DROP FOREIGN KEY name` clauses separated only by commas,
  rejecting any other comma clause;
- for each ADD clause, prove the referenced table is ordinary non-generated;
- for each DROP clause, resolve the existing FK's referenced table before
  execution and apply the same generated-column guard.

Successful pure add lists reuse
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_ADD_FOREIGN_KEY`; successful
pure drop lists reuse
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_DROP_FOREIGN_KEY`. Both stay
metadata-only and keep the native file-operation checkpoint-needed marker clear.

## Compatibility Impact

This does not broaden SQL grammar beyond MariaDB. It makes MyLite's ownerless
post-native dictionary publication boundary cover a MariaDB-compatible
multi-clause FK ALTER shape that applications can issue as one statement. Mixed
ALTER lists remain conservative until their native file or metadata effects are
classified separately.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. The slice affects only
the MyLite ownerless dictionary recovery classifier and hook-test evidence for
metadata-only FK ALTER completion. InnoDB remains responsible for native FK
dictionary changes.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The slice
adds parser helpers, focused hook tests, CTest registration, and documentation.

## Test And Verification Plan

- Add selectors:
  `dictionary-foreign-key-multi-add-crash` and
  `dictionary-foreign-key-multi-drop-crash`.
- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the two new selectors directly and through CTest.
- Run adjacent single-FK add/drop crash selectors.
- Run production build guards, format-check, and `git diff --check`.

## Acceptance Criteria

- Pure multi-FK ADD statements are marked with the existing metadata-only FK
  ADD recovery kind.
- Pure multi-FK DROP statements are marked with the existing metadata-only FK
  DROP recovery kind.
- Mixed comma-separated FK/non-FK ALTER lists are not claimed by this slice.
- A crash after native multi-FK ADD and before ownerless dictionary finish
  recovers both constraints while a live peer remains open, with the native
  file-op marker clear.
- A crash after native multi-FK DROP and before ownerless dictionary finish
  recovers both absent constraints while a live peer remains open, with the
  native file-op marker clear.
- Ownerless reopen, native reopen, and forced `.shm` rebuild observe the
  recovered FK state.

## Risks And Follow-Up

- This remains a token-level recognizer, not a replacement for MariaDB's parser.
- Mixed FK/column/index ALTER lists still need a separate design because they
  may require native file-operation checkpoint evidence.
- Generated-column FK live recovery remains conservative and planned.
