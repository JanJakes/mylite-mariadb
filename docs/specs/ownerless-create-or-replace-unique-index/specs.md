# Ownerless CREATE OR REPLACE UNIQUE INDEX

## Problem

Ownerless standalone unique-index DDL coverage proved that a multi-column
unique index created by another process refreshes an already-open peer, enforces
duplicate-key rejection, and disappears after `DROP INDEX`. MariaDB also
supports `CREATE OR REPLACE UNIQUE INDEX`, which keeps the index name while
replacing the unique key definition. That spelling must move both metadata and
duplicate-key enforcement for ownerless peers.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` has separate grammar arms for
  `create_or_replace INDEX` and `create_or_replace UNIQUE INDEX`; the unique
  arm calls `Lex->add_create_index(Key::UNIQUE, ...)`.
- `mariadb/sql/sql_lex.h:add_create_index()` stores the DDL options on the new
  `Key` and rejects incompatible `OR REPLACE` plus `IF NOT EXISTS` options.
- `mariadb/sql/sql_table.cc` handles `key->or_replace()` by adding the
  existing key name to the alter drop list before adding the replacement key.
- Existing ownerless unique-index DDL coverage already keeps a peer handle open
  across child-process create/drop boundaries, verifies
  `INFORMATION_SCHEMA.STATISTICS.NON_UNIQUE = 0`, tests duplicate-key errno
  `1062`, and reopens after forced `.shm` rebuild.

## Design

Extend the `unique-index-ddl` ownerless selector with a replacement boundary:

- create a table and a unique index named `ownerless_unique_tenant_slug` over
  `(tenant_id, slug)`,
- verify an already-open peer sees the two unique key parts and rejects a
  duplicate `(tenant_id, slug)` write,
- insert a non-conflicting row through the peer,
- execute `CREATE OR REPLACE UNIQUE INDEX ownerless_unique_tenant_slug` over
  `(tenant_id, weight)` from the child process,
- verify the same peer sees the index name moved from `slug` to `weight`,
  accepts the formerly duplicate `(tenant_id, slug)` shape, and rejects a
  duplicate `(tenant_id, weight)` shape,
- drop the index and verify a duplicate `(tenant_id, weight)` shape is accepted
  after the peer observes final index absence.

## Compatibility Impact

This covers a supported MariaDB standalone unique-index DDL spelling for the
ownerless read/write mode. It does not broaden `FULLTEXT`, `SPATIAL`,
algorithm/lock-option matrices, or crash-recovery claims for unique-index
replacement.

## Directory And Lifecycle Impact

The slice adds no new durable MyLite files. It exercises MariaDB/InnoDB native
secondary-index metadata inside the MyLite database directory and verifies final
state through ownerless and ordinary native reopen before and after forced
volatile shared-memory rebuild.

## Native Storage Impact

No storage format changes. The test relies on MariaDB/InnoDB's native unique
secondary-index replacement and duplicate-key enforcement.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `unique-index-ddl` selector.
- Run the matching embedded ownerless cross-process SQL CTest shard.
- Build and run the focused `unique-index-ddl` selector in
  `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The already-open peer observes the original unique index over
  `(tenant_id, slug)` and rejects duplicate old-key writes.
- After `CREATE OR REPLACE UNIQUE INDEX`, the peer observes the same index name
  over `(tenant_id, weight)` and no longer over `slug`.
- The peer accepts the formerly duplicate old-key shape after replacement.
- The peer rejects duplicate replacement-key writes until `DROP INDEX`.
- Final duplicate old-key and replacement-key row shapes plus index absence
  survive ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- Crash recovery during `CREATE OR REPLACE UNIQUE INDEX` remains a follow-up
  hook-build class.
- Broader algorithm/lock-option matrices and randomized external MariaDB/RQG
  oracle stress remain planned.
