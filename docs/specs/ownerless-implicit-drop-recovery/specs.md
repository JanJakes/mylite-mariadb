# Ownerless Implicit Drop Recovery

## Problem Statement

Ownerless hook coverage already kills `DROP TABLE schema.table` writers after
native file removal but before MyLite dictionary finish. MariaDB also accepts
single-table drops resolved through the session default database:

```sql
USE app;
DROP TABLE table_name;
```

MyLite's focused ownerless recovery classifier was stricter than MariaDB's
syntax and only marked the explicit `DROP TABLE schema.table` shape as the
drop recovery kind. The same-statement multi-drop matrix remains separate
because prior exploration hit a native InnoDB purge assertion before the MyLite
`dictionary-before-finish` hook.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `DROP TABLE` with a table list whose table
  names can be resolved against the session default database.
- `mariadb/sql/sql_parse.cc` routes `SQLCOM_DROP_TABLE` through the normal
  DDL execution path.
- `mariadb/sql/sql_table.cc::mysql_rm_table()` removes base-table metadata and
  calls storage-engine delete paths for native files.
- `packages/libmylite/src/database.cc` classifies supported dictionary DDL
  before SQL execution and marks the matching ownerless recovery kind before
  `dictionary-before-finish`.

## Scope And Non-Goals

In scope:

- Broaden focused ownerless drop recovery classification from a required
  two-part identifier to a single one- or two-part table identifier.
- Add unsafe-hook coverage for `USE app; DROP TABLE table_name` with a live
  ownerless peer.
- Verify live marker retention, final no-live marker drain, forced `.shm`
  rebuild, and ordinary native reopen.

Out of scope:

- Same-statement multi-drop or cross-schema multi-drop crash recovery.
- `DROP TABLE IF EXISTS` no-op matrices.
- Temporary-table drop recovery.
- New file formats, public APIs, or native storage changes.

## Design

Reuse the one- or two-part table identifier parser used by rename and truncate
recovery. The drop classifier still requires exactly one table identifier:

- `DROP TABLE`
- one- or two-part table identifier
- only trailing semicolons after the identifier

This intentionally rejects comma-separated drop lists for this slice. Matching
statements keep using `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_DROP_TABLE`.

The hook test creates `app.ownerless_implicit_drop_crash`, starts a live
ownerless peer, opens a writer with `USE app`, arms `dictionary-before-finish`,
and executes:

```sql
DROP TABLE ownerless_implicit_drop_crash
```

The parent kills the writer after native file removal but before ownerless
dictionary finish.

## Compatibility Impact

This aligns focused ownerless crash-recovery classification with MariaDB's
single-table implicit-schema drop spelling. Normal SQL semantics remain
MariaDB-owned.

## Directory And Lifecycle Impact

No directory layout changes. The existing native file-operation checkpoint
marker remains durable while a live peer remains open and drains only on final
no-live ownerless close after native checkpoint proof.

## Native Storage Impact

No storage-format changes. Native InnoDB file removal remains the storage
authority.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds a bounded parser expansion and focused test
coverage.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selector `dictionary-implicit-drop-crash`.
- Run adjacent drop selectors:
  `dictionary-drop-file-op-marker-crash` and `dictionary-drop-crash`.
- Run the registered hook CTest entry.
- Build production ownerless SQL target and run representative non-hook drop
  coverage.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The implicit drop writer reaches `dictionary-before-finish` and is killed
  before ownerless dictionary finish.
- A live peer keeps the native file-operation checkpoint marker durable.
- Ownerless reopen while the peer is live observes the dropped table and files
  absent.
- Final no-live ownerless close drains the marker after native checkpoint
  proof.
- Forced `.shm` rebuild and ordinary native reopen preserve the absent table.

## Verification Results

- Hook build: `dictionary-implicit-drop-crash`.
- Hook build adjacent selectors: `dictionary-drop-file-op-marker-crash` and
  `dictionary-drop-crash`.
- Hook CTest: `libmylite.ownerless-dictionary-implicit-drop-crash`.
- Production embedded build: `ddl-broader` and `table-idempotent-ddl`.

## Risks And Follow-Up

- Focused coverage intentionally stays on a single dropped table. Same-statement
  multi-drop and cross-schema multi-drop remain planned because the previous
  attempted selector hit a native InnoDB purge assertion before the MyLite hook.
- `DROP TABLE IF EXISTS` no-op and temporary-table drop matrices remain planned.
- Broader DDL/file-lifecycle recovery, active-reader pressure crash/oracle
  breadth, and external MariaDB/RQG stress remain open completion work.
