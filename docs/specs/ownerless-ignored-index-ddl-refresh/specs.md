# Ownerless Ignored Index DDL Refresh

## Problem

MariaDB supports changing whether an index is considered by the optimizer with
`ALTER TABLE ... ALTER INDEX ... IGNORED` and `NOT IGNORED`. Ownerless DDL
coverage already proves secondary-index create, drop, and rename refresh peers,
but index ignorability is a separate metadata path. Ownerless mode needs
evidence that an already-open peer observes the ignored/not-ignored state and
that the final usable index survives reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... ALTER INDEX index_name IGNORED` and
  `ALTER TABLE ... ALTER INDEX index_name NOT IGNORED` through the
  alter-table grammar and records `ALTER_INDEX_IGNORABILITY`.
- `mariadb/sql/sql_table.cc` maps the alter-list entry to altered index
  metadata and updates the key's `is_ignored` flag.
- `mariadb/storage/innobase/handler/handler0alter.cc` includes
  `ALTER_INDEX_IGNORABILITY` in InnoDB alter handling.
- `mariadb/sql/sql_show.cc` exposes index ignorability through the `IGNORED`
  column in `information_schema.statistics`.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for `ALTER INDEX ... IGNORED` and
  `ALTER INDEX ... NOT IGNORED`.
- Verify an already-open ownerless peer observes `IGNORED = 'YES'` and later
  `IGNORED = 'NO'` through `information_schema.statistics`.
- Verify ordinary DML continues while the index is ignored and that the final
  not-ignored index can be used with `FORCE INDEX`.
- Verify final metadata and rows survive ownerless reopen, ordinary exclusive
  reopen, forced `.shm` rebuild, and exclusive reopen after rebuild.
- Do not claim optimizer plan equivalence or every index metadata option.

## Design

Add an `ignored-index-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. A child ownerless process creates an InnoDB table, inserts rows, creates a
   secondary index, and signals the parent.
2. The already-open parent verifies `IGNORED = 'NO'` and forced-index reads.
3. The child marks the index `IGNORED`.
4. The parent verifies `IGNORED = 'YES'` and writes another row.
5. The child marks the index `NOT IGNORED`.
6. The parent verifies `IGNORED = 'NO'`, writes another row, and verifies
   forced-index reads over the final data.
7. Reopen helper assertions verify the not-ignored index and final rows through
   ownerless/native reopen before and after forced `.shm` rebuild.

## Compatibility Impact

No public API behavior changes. The slice strengthens partial ownerless DDL
compatibility for a MariaDB index-metadata operation while broader DDL and
external oracle coverage remain partial.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing native InnoDB table
metadata and ownerless concurrency files under the MyLite-owned directory.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. MyLite only coordinates the
ownerless dictionary boundary and verifies durable reopen behavior for the
resulting metadata.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `ignored-index-ddl` in `embedded-dev`.
- Build and run focused `ignored-index-ddl` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- The already-open peer observes `IGNORED = 'YES'` after the child marks the
  index ignored.
- The already-open peer observes `IGNORED = 'NO'` after the child restores the
  index.
- Final forced-index reads and row aggregates survive ownerless/native reopen
  before and after forced `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This does not assert optimizer plan choices while the index is ignored.
- Broader randomized DDL oracles remain planned.
