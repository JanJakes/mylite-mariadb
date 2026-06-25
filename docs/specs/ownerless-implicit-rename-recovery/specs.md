# Ownerless Implicit Rename Recovery

## Problem Statement

Ownerless hook coverage already kills explicit schema-qualified
`RENAME TABLE schema.source TO schema.target` writers after native file
movement but before MyLite finishes dictionary publication. The remaining
rename gap called out in the cross-process concurrency plan is the equivalent
implicit-schema spelling:

```sql
USE app;
RENAME TABLE source TO target;
```

MariaDB resolves the missing schema through the session default database before
running the same native rename path. MyLite's ownerless recovery classifier was
more restrictive than MariaDB's supported syntax and only marked fully
qualified rename pairs as recoverable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` sets `SQLCOM_RENAME_TABLE` for `RENAME TABLE`.
- `mariadb/sql/sql_parse.cc::mysql_execute_command()` validates the rename
  list with `check_rename_table()` and calls `mysql_rename_tables()`.
- `mariadb/sql/sql_rename.cc::mysql_rename_tables()` walks the rename list,
  logs DDL rename state, and calls `mysql_rename_table()`.
- `mariadb/sql/sql_table.cc::mysql_rename_table()` calls the storage engine's
  `ha_rename_table()` after building the old and new table paths.
- `packages/libmylite/src/database.cc` classifies supported dictionary DDL
  statements before executing SQL so a killed ownerless writer can be recovered
  with the matching dictionary recovery kind.

## Scope And Non-Goals

In scope:

- Broaden ownerless `RENAME TABLE` dictionary recovery classification from
  only two-part `schema.table` identifiers to one- or two-part identifiers in
  each rename pair.
- Add focused unsafe-hook coverage for an implicit-schema single-pair rename
  with a live ownerless peer.
- Verify marker retention while the peer remains live, final no-live marker
  drain, forced `.shm` rebuild, and ordinary native reopen.

Out of scope:

- `ALTER TABLE ... RENAME TO` native-loop variants.
- Temporary-table, view-only, or unsupported object rename matrices.
- SQL-level table-lock fault injection.
- New file formats, public APIs, or native storage changes.

## Design

Replace the fully qualified rename classifier with a small pair parser that
accepts a table identifier as either:

- one part: `table`
- two parts: `schema.table`

Each `RENAME TABLE` pair must still be `old_identifier TO new_identifier`, and
additional pairs must be comma separated. Only trailing semicolons are accepted
after the list. Matching statements keep using the existing
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_RENAME_TABLE` recovery kind.

The hook test creates `app.ownerless_implicit_rename_crash_source`, starts a
live ownerless peer, opens a writer with `USE app`, arms
`dictionary-before-finish`, and executes:

```sql
RENAME TABLE ownerless_implicit_rename_crash_source
TO ownerless_implicit_rename_crash_target
```

The parent kills the writer after the hook signals that native rename work has
completed but before MyLite dictionary finish.

## Compatibility Impact

This aligns MyLite's ownerless crash-recovery classification with MariaDB's
accepted implicit-schema `RENAME TABLE` spelling. Normal SQL semantics remain
MariaDB-owned.

## Directory And Lifecycle Impact

No directory layout changes. The existing native file-operation checkpoint
marker remains durable while a peer process is live, and the final no-live
ownerless close must drain the marker only after native checkpoint proof.

## Native Storage Impact

No storage-format changes. The slice relies on MariaDB/InnoDB's native rename
path and the existing MyLite ownerless file-operation recovery marker.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds one bounded parser branch and focused test
coverage.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selector `dictionary-implicit-rename-crash`.
- Run adjacent rename selectors:
  `dictionary-rename-file-op-marker-crash`,
  `dictionary-cross-schema-rename-crash`,
  `dictionary-multi-rename-crash`, and
  `dictionary-cross-schema-multi-rename-crash`.
- Run the registered hook CTest entry.
- Build production ownerless SQL target and run representative non-hook rename
  selectors where applicable.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The implicit-schema `RENAME TABLE` writer reaches `dictionary-before-finish`
  and is killed before ownerless dictionary finish.
- A live peer keeps the native file-operation checkpoint marker durable.
- Ownerless reopen while the peer is live observes the target table, not the
  source table, and can write the target table.
- Final no-live ownerless close drains the marker after native checkpoint
  proof.
- Forced `.shm` rebuild and ordinary native reopen preserve the renamed table
  and row aggregate.

## Verification Results

- Hook build: `dictionary-implicit-rename-crash`.
- Hook build adjacent selectors:
  `dictionary-rename-file-op-marker-crash`,
  `dictionary-cross-schema-rename-crash`,
  `dictionary-multi-rename-crash`, and
  `dictionary-cross-schema-multi-rename-crash`.
- Hook CTest: `libmylite.ownerless-dictionary-implicit-rename-crash`.
- Production embedded build: `cross-schema-rename` and
  `multi-rename-cycle`.

## Risks And Follow-Up

- The classifier now accepts mixed one- and two-part rename pair identifiers
  because MariaDB resolves them through the same command path, but the focused
  test covers the all-implicit single-pair shape.
- `ALTER TABLE ... RENAME TO`, temporary-table, view-only, `IF EXISTS`, and
  native-loop crash rename variants remain planned.
- Broader DDL/file-lifecycle recovery, active-reader pressure crash/oracle
  breadth, and external MariaDB/RQG stress remain open completion work.
