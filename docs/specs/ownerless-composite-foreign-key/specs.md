# Ownerless Composite Foreign Keys

## Problem

Ownerless foreign-key action coverage now proves representative single-column
referential actions across processes. It still does not prove that InnoDB
foreign-key identity remains correct when the referenced key is composite and
different parent rows share one component of the key. Composite keys exercise
multi-column secondary-index lookup, dictionary metadata, cascaded update
vectors, and peer page visibility across more than one key field.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `FOREIGN KEY (...) REFERENCES ... (...)`
  with comma-separated column lists for both child and parent key columns.
- `mariadb/storage/innobase/handler/handler0alter.cc` builds
  `dict_foreign_t` structures from the parsed column lists and validates that
  indexes can satisfy all foreign-key fields.
- `mariadb/storage/innobase/include/dict0mem.h` stores `dict_foreign_t::n_fields`
  plus child and referenced column arrays, so composite constraints are a
  first-class InnoDB dictionary shape rather than repeated single-column
  constraints.
- `mariadb/storage/innobase/row/row0ins.cc` checks all fields in a foreign-key
  entry, suppresses checks only when a foreign-key field is SQL `NULL`, and
  calculates cascaded update vectors over the foreign-key field count.
- MyLite ownerless peer visibility must refresh dictionary metadata and
  page-version state so an already-open peer observes both composite-key
  enforcement and cascaded child-key updates committed by another process.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for a composite parent primary key.
- Composite child foreign-key enforcement with a missing composite parent.
- `ON UPDATE CASCADE` over one component of a composite referenced key.
- Tenant-isolation proof where another parent row shares the updated numeric
  key component under a different tenant key.
- `ON DELETE RESTRICT` over the composite key.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Composite `SET NULL` matrices.
- Supported stored generated-column foreign keys; that shape is covered
  separately by
  `docs/specs/ownerless-generated-column-foreign-key/specs.md`.
- Cyclic foreign-key graphs.
- Deep linear cascade chains; that shape is covered separately by
  `docs/specs/ownerless-foreign-key-deep-cascade/specs.md`.
- Concurrent composite-FK deadlock matrices.
- Crash injection while a composite referential action is in progress.

## Design

- Add a focused `composite-foreign-key` selector to
  `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates a parent table with primary key
  `(tenant_id, id)` and a child table with foreign key
  `(tenant_id, parent_id)` referencing it.
- The parent keeps an already-open ownerless peer handle, verifies the
  composite constraint metadata, rejects an insert whose composite parent key is
  missing, and inserts another valid row for a different tenant.
- The child updates only tenant 1's referenced key from `(1, 10)` to `(1, 11)`.
  The parent peer verifies tenant 1's child row cascaded to `parent_id=11` and
  tenant 2's child rows still reference `(2, 10)`.
- The child attempts to delete tenant 2's referenced parent row and verifies
  MariaDB errno 1451 because two child rows still reference that composite key.
- The test closes peers, validates final state through ownerless read/write
  and ordinary native exclusive reopen, deletes `.shm`, and validates both
  reopen paths again.

## Compatibility Impact

This adds bounded ownerless evidence for composite InnoDB foreign keys. It
does not claim complete foreign-key compatibility; generated-column keys,
cyclic cascades, and crash-in-action recovery remain separate work.

## Directory And Lifecycle Impact

No new files or layout changes. The slice uses native InnoDB files inside the
MyLite database directory and validates final state after volatile shared-memory
rebuild.

## Native Storage Impact

No storage format changes. The test uses MariaDB/InnoDB's native composite
foreign-key dictionary and enforcement paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `composite-foreign-key` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes a composite foreign-key constraint
  created by another process.
- A missing composite parent insert fails with MariaDB errno 1452.
- A parent-key update committed by another process cascades only the matching
  tenant-scoped child rows.
- A restricted delete over the composite parent key fails with MariaDB errno
  1451 and leaves the parent/child rows intact.
- Final composite-FK rows and metadata survive ownerless/native reopen before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves a small deterministic composite-key shape. Unsupported
  generated-column FK variants, cyclic graphs, and crash injection inside
  composite referential-action execution remain future work.
