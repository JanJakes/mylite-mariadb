# Ownerless Temporary Table Truncate Tracking

## Problem Statement

Ownerless temporary-table tracking now handles create, drop, and rename forms.
One remaining temporary-table DDL boundary is `TRUNCATE TABLE` against a
connection-local temporary table that shadows a permanent table of the same
name. MariaDB truncates only the temporary table and keeps the temporary table
present. MyLite must keep the name classified as temporary after truncate so
the same handle does not accidentally refresh/read the permanent table until
the temporary table is dropped.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/temporary_tables.cc:115-138` finds temporary tables in the
  connection-local temporary table list.
- `mariadb/sql/sql_parse.cc:6346-6418` records the temporary/base table type
  distinction for rename handling; the same connection-local distinction is
  used by later statements that resolve a shadowed table name.
- `mariadb/sql/sql_truncate.cc:510-532` reaches native truncate/recreate after
  SQL preflight and table resolution. For a temporary table, the resolved
  object is session-local and must not mutate the permanent table with the
  same qualified name.
- `packages/libmylite/src/database.cc` keeps a handle-local ownerless
  temporary table name set that forces conservative native handling for
  statements touching tracked temporary names.

## Design

Add a production SQL selector named `temporary-table-truncate-tracking` and a
focused CTest named `libmylite.ownerless-temporary-table-truncate-tracking`.

The selector:

1. Creates a permanent InnoDB table.
2. Opens an ownerless handle, creates a same-named temporary table, and inserts
   temporary rows.
3. Runs `TRUNCATE TABLE` against that name and verifies the temporary table is
   empty.
4. Updates the permanent table from a peer handle.
5. Verifies the original handle still sees the empty temporary table, proving
   truncate did not clear temporary tracking.
6. Inserts a temporary row, drops the temporary table, and verifies the same
   handle now sees the peer-updated permanent table.
7. Verifies ownerless reopen, forced `.shm` rebuild, and ordinary native reopen
   preserve only the permanent table state.

## Scope And Non-Goals

In scope:

- Production ownerless SQL coverage for temporary-table truncate tracking.
- A same-name temporary/permanent shadowing case.
- Peer update visibility after the temporary table is dropped.
- Ownerless, forced-`.shm`, and ordinary native reopen oracles for the
  permanent table.

Out of scope:

- Hook crash recovery during temporary-table truncate.
- Multi-table temporary DDL lists.
- Partitioned temporary tables.
- Positive permanent-table truncate file-lifecycle recovery, which is covered
  separately.

## Compatibility Impact

No SQL behavior changes are expected. The selector records MariaDB-compatible
temporary-table semantics: `TRUNCATE TABLE` on a session-local temporary table
does not affect a permanent table with the same name and does not remove the
temporary table from the session.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. Temporary InnoDB files remain in
the process runtime area. The durable MyLite database directory keeps only the
permanent table's state, which is verified after ownerless/native reopen and
forced `.shm` rebuild.

## Public API, Build, Size, License

No public API, dependency, binary-size, or license changes. The slice adds one
production selector, one CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run direct selector `temporary-table-truncate-tracking`.
- Run the focused CTest
  `libmylite.ownerless-temporary-table-truncate-tracking`.
- Run adjacent temporary selectors:
  `temporary-table-rename-tracking`,
  `sql-case test_ownerless_temporary_tablespace_allows_peer_temp_tables`,
  `sql-case test_crashed_ownerless_temporary_table_peer_is_recovered`, and
  `temp-stress`.
- Run ownerless temporary stress smoke, format check, CI production-build
  audit, and `git diff --check`.

## Acceptance Criteria

- `TRUNCATE TABLE` empties the temporary table while preserving its tracked
  temporary identity.
- Peer updates to the permanent table stay hidden behind the still-present
  temporary table on the original handle.
- Dropping the temporary table exposes the peer-updated permanent table on the
  original handle.
- Ownerless reopen, forced `.shm` rebuild, and ordinary native reopen preserve
  the permanent table state.

## Risks And Follow-Up

- Hook crash recovery during temporary-table truncate is covered separately by
  `ownerless-temporary-truncate-crash-recovery`.
- Broader temporary DDL matrices and randomized DDL external oracle stress
  remain planned.
