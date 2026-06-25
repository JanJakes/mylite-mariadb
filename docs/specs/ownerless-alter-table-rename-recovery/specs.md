# Ownerless Alter Table Rename Recovery

## Problem Statement

Ownerless recovery now covers `RENAME TABLE` forms with explicit and
implicit-schema table identifiers. MariaDB also supports table rename through
the ALTER path:

```sql
ALTER TABLE app.source RENAME TO app.target;
```

That spelling reaches MariaDB's native table rename/file-movement logic but was
not classified as a recoverable ownerless rename boundary. If a writer dies
after native rename work but before MyLite dictionary finish, live-peer
recovery must treat it like the equivalent `RENAME TABLE` operation.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... RENAME opt_to
  table_ident` and sets `ALTER_RENAME`.
- `mariadb/sql/sql_alter.cc::Sql_cmd_alter_table::execute()` requires DROP
  privilege for `ALTER_RENAME`, matching `RENAME TABLE` privilege needs.
- `mariadb/sql/sql_table.cc::mysql_alter_table()` handles ALTER rename before
  the normal copy/in-place ALTER table path; the source comment notes that
  `RENAME TABLE` is the ALTER clause supported for views, while base-table
  ALTER continues through table rename handling.
- `mariadb/sql/sql_table.cc::mysql_rename_table()` calls the storage engine's
  `ha_rename_table()` after building the old and new table paths.
- `packages/libmylite/src/database.cc` classifies supported dictionary DDL
  before SQL execution and publishes the matching ownerless dictionary recovery
  kind before `dictionary-before-finish`.

## Scope And Non-Goals

In scope:

- Classify `ALTER TABLE <table> RENAME [TO|AS|=] <table>` as the existing
  ownerless rename recovery kind when the table names are one- or two-part
  identifiers.
- Add focused unsafe-hook coverage for `ALTER TABLE schema.table RENAME TO
  schema.table` with a live ownerless peer.
- Verify live marker retention, final no-live marker drain, forced `.shm`
  rebuild, and ordinary native reopen.

Out of scope:

- View-only ALTER rename recovery.
- Temporary-table rename, `IF EXISTS`, or other rename variants.
- Broader rebuilt/truncated/dropped file-lifecycle classes.
- New file formats, public APIs, or native storage changes.

## Design

Reuse the same rename identifier parser used for `RENAME TABLE`. The ALTER
classifier requires:

- `ALTER TABLE`
- an old one- or two-part table identifier
- `RENAME`
- optional `TO`, `AS`, or `=`
- a new one- or two-part table identifier
- only trailing semicolons after the target

This rejects `ALTER TABLE ... RENAME COLUMN` and `ALTER TABLE ... RENAME
INDEX` because those statements leave non-semicolon tokens after the first
post-`RENAME` identifier.

Matching statements use `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE`.
No new recovery state or file format is needed.

## Compatibility Impact

This aligns ownerless crash-recovery classification with MariaDB's ALTER table
rename syntax for base InnoDB tables. Normal SQL semantics remain
MariaDB-owned.

## Directory And Lifecycle Impact

No directory layout changes. The native file-operation checkpoint marker stays
durable while a live peer remains open and drains only on final no-live
ownerless close after native checkpoint proof.

## Native Storage Impact

No storage-format changes. InnoDB native rename/file-operation redo remains the
storage authority.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds a bounded classifier branch plus focused
coverage.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selector `dictionary-alter-table-rename-crash`.
- Run adjacent rename selectors:
  `dictionary-implicit-rename-crash`,
  `dictionary-rename-file-op-marker-crash`, and
  `dictionary-cross-schema-rename-crash`.
- Run the registered hook CTest entry.
- Build production ownerless SQL target and run representative non-hook ALTER
  and rename selectors.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The ALTER rename writer reaches `dictionary-before-finish` and is killed
  before ownerless dictionary finish.
- A live peer keeps the native file-operation checkpoint marker durable.
- Ownerless reopen while the peer is live observes the target table, not the
  source table, and can write the target table.
- Final no-live ownerless close drains the marker after native checkpoint
  proof.
- Forced `.shm` rebuild and ordinary native reopen preserve the renamed table
  and row aggregate.

## Verification Results

- Hook build: `dictionary-alter-table-rename-crash`.
- Hook build adjacent selectors: `dictionary-implicit-rename-crash`,
  `dictionary-rename-file-op-marker-crash`, and
  `dictionary-cross-schema-rename-crash`.
- Hook CTest: `libmylite.ownerless-dictionary-alter-table-rename-crash`.
- Production embedded build: `cross-schema-rename`, `multi-rename-cycle`, and
  `rename-index-ddl`.

## Risks And Follow-Up

- Focused coverage uses the `RENAME TO` spelling over a base InnoDB table; view
  rename, temporary-table rename, and other rename variants remain planned.
- Broader DDL/file-lifecycle recovery, active-reader pressure crash/oracle
  breadth, and external MariaDB/RQG stress remain open completion work.
