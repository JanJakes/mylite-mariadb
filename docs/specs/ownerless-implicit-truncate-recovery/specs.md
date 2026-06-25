# Ownerless Implicit Truncate Recovery

## Problem Statement

Ownerless hook coverage already kills `TRUNCATE TABLE schema.table` writers
after native truncate/recreate work but before MyLite dictionary finish. MariaDB
also accepts `TRUNCATE table` because `TABLE` is optional and the table name can
be resolved through the session default database.

MyLite's ownerless recovery classifier was stricter than MariaDB's syntax and
only marked the explicit `TRUNCATE TABLE schema.table` spelling as the focused
truncate recovery kind.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `TRUNCATE_SYM opt_table_sym table_name`,
  where `opt_table_sym` may be empty or `TABLE`.
- `mariadb/sql/sql_parse.cc` treats `SQLCOM_TRUNCATE` as data-changing,
  write-logged, and disallowed in read-only transactions.
- `mariadb/sql/sql_truncate.cc::Sql_cmd_truncate_table::execute()` delegates to
  `truncate_table()`.
- `mariadb/sql/sql_truncate.cc::truncate_table()` uses handler truncate or
  truncate-by-recreate paths, both of which are native storage file-lifecycle
  work for InnoDB.
- `packages/libmylite/src/database.cc` classifies supported dictionary DDL
  before SQL execution and marks the matching ownerless recovery kind before
  `dictionary-before-finish`.

## Scope And Non-Goals

In scope:

- Broaden ownerless truncate recovery classification to accept optional
  `TABLE` and one- or two-part table identifiers.
- Add focused unsafe-hook coverage for `USE app; TRUNCATE table` with a live
  ownerless peer.
- Verify marker retention while the peer remains live, final no-live marker
  drain, forced `.shm` rebuild, and ordinary native reopen.

Out of scope:

- Partition truncate or unsupported partition lifecycle paths.
- Foreign-key-restricted truncate matrices.
- Broader multi-table/cross-schema drop recovery.
- New file formats, public APIs, or native storage changes.

## Design

Reuse the same one- or two-part table identifier parser used for rename
recovery. The truncate classifier requires:

- `TRUNCATE`
- optional `TABLE`
- one- or two-part table identifier
- only trailing semicolons after the identifier

Matching statements keep using
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TRUNCATE_TABLE`. No new recovery state or
file format is needed.

The hook test creates `app.ownerless_implicit_truncate_crash`, starts a live
ownerless peer, opens a writer with `USE app`, arms `dictionary-before-finish`,
and executes:

```sql
TRUNCATE ownerless_implicit_truncate_crash
```

The parent kills the writer after native truncate work has completed but before
ownerless dictionary finish.

## Compatibility Impact

This aligns ownerless crash-recovery classification with MariaDB's accepted
implicit-schema truncate spelling. Normal SQL semantics remain MariaDB-owned.

## Directory And Lifecycle Impact

No directory layout changes. The existing native file-operation checkpoint
marker remains durable while a live peer remains open and drains only on final
no-live ownerless close after native checkpoint proof.

## Native Storage Impact

No storage-format changes. InnoDB's native truncate/recreate work remains the
storage authority.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds a bounded parser expansion and focused test
coverage.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selector `dictionary-implicit-truncate-crash`.
- Run adjacent truncate selectors:
  `dictionary-truncate-file-op-marker-crash` and
  `dictionary-truncate-crash`.
- Run the registered hook CTest entry.
- Build production ownerless SQL target and run representative non-hook
  truncate coverage.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The implicit truncate writer reaches `dictionary-before-finish` and is killed
  before ownerless dictionary finish.
- A live peer keeps the native file-operation checkpoint marker durable.
- Ownerless reopen while the peer is live observes the truncated table and can
  write it.
- Final no-live ownerless close drains the marker after native checkpoint
  proof.
- Forced `.shm` rebuild and ordinary native reopen preserve the post-truncate
  row aggregate.

## Verification Results

- Hook build: `dictionary-implicit-truncate-crash`.
- Hook build adjacent selectors: `dictionary-truncate-file-op-marker-crash`
  and `dictionary-truncate-crash`.
- Hook CTest: `libmylite.ownerless-dictionary-implicit-truncate-crash`.
- Production embedded build: `ddl-truncate-refresh` and `ddl-broader`.

## Risks And Follow-Up

- Focused coverage uses `TRUNCATE table` with a session default database;
  partition truncate and foreign-key edge matrices remain planned.
- Broader DDL/file-lifecycle recovery, active-reader pressure crash/oracle
  breadth, and external MariaDB/RQG stress remain open completion work.
