# Ownerless CREATE OR REPLACE INDEX

## Problem

Ownerless standalone index DDL coverage already proved peer refresh for
`CREATE INDEX IF NOT EXISTS`, duplicate index-name failure, missing
`DROP INDEX IF EXISTS`, repeated real-index drop, and final reopen behavior.
MariaDB also supports `CREATE OR REPLACE INDEX`, which keeps the index name but
replaces the existing key definition. That path exercises native secondary-index
metadata replacement without changing table rows.

## Source Findings

- MariaDB 11.8 grammar accepts `create_or_replace INDEX` and
  `create_or_replace UNIQUE INDEX` in `mariadb/sql/sql_yacc.yy`.
- MariaDB `sql_table.cc` handles `or_replace()` by adding the existing key to
  the drop list before adding the replacement key.
- The existing ownerless `index-idempotent-ddl` selector already keeps a peer
  open across each standalone index DDL boundary and verifies final
  ownerless/native reopen behavior.

## Design

Extend the ownerless index-idempotent DDL sequence:

- create a table and a secondary index over `value`,
- verify an already-open ownerless peer observes the original key part,
- run duplicate non-idempotent and idempotent create checks,
- execute `CREATE OR REPLACE INDEX` with the same index name over `note`,
- verify the peer observes the key-part replacement and can use the replaced
  index,
- continue through missing-index `DROP INDEX IF EXISTS`, repeated real drop,
  final ownerless/native reopen, forced `.shm` rebuild, and native reopen.

## Compatibility Impact

This adds coverage for a supported MariaDB standalone-index DDL spelling. It
does not change SQL policy or broaden unsupported special-index support.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test`.
- Run the focused `index-idempotent-ddl` selector.
- Run the matching embedded ownerless SQL CTest shard.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- An already-open ownerless peer sees the original `value` key part.
- After `CREATE OR REPLACE INDEX`, the same peer sees the same index name over
  `note` and no longer sees it over `value`.
- The replacement index is usable through `FORCE INDEX`.
- Final repeated `DROP INDEX IF EXISTS` leaves the index absent through
  ownerless and ordinary native reopen before and after forced `.shm` rebuild.
