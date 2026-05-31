# Ownerless Foreign Key Actions

## Problem

Ownerless foreign-key coverage currently proves create-time cascade delete and
`ALTER TABLE` add/drop enforcement refresh. It does not yet prove a broader
referential-action matrix across already-open ownerless processes. Parent-row
updates and deletes can cascade, set child columns to `NULL`, or be rejected by
restrict/no-action rules, and those effects must become visible through peer
page-version reads and native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ON UPDATE` and `ON DELETE` foreign-key
  options and maps `RESTRICT`, `CASCADE`, and `SET NULL` to foreign-key option
  values.
- `mariadb/storage/innobase/handler/handler0alter.cc` translates those parsed
  options into InnoDB `dict_foreign_t` flags such as `DELETE_CASCADE`,
  `UPDATE_CASCADE`, and `DELETE_SET_NULL`.
- `mariadb/storage/innobase/include/dict0mem.h` documents that `RESTRICT`
  means no action flag in the InnoDB foreign-key type bits.
- `mariadb/storage/innobase/row/row0ins.cc` builds and executes cascade update
  or delete nodes for child tables, calculates `SET NULL` and `CASCADE`
  update vectors, and returns foreign-key errors when no action is allowed.
- MyLite ownerless commits publish dirty pages into the page-version WAL before
  page-visible LSN publication, and ownerless statement refresh makes peer
  committed pages visible to already-open handles.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for `ON UPDATE CASCADE`.
- Cross-process ownerless SQL coverage for `ON DELETE CASCADE`.
- Cross-process ownerless SQL coverage for `ON DELETE SET NULL`.
- Cross-process ownerless SQL coverage for `ON DELETE RESTRICT`/no-action
  rejection.
- Final ownerless and native exclusive reopen checks before and after forced
  `.shm` rebuild.

Out of scope:

- Composite foreign keys.
- Supported stored generated-column foreign keys; that shape is covered
  separately by
  `docs/specs/ownerless-generated-column-foreign-key/specs.md`.
- Deep linear cascade chains; that shape is covered separately by
  `docs/specs/ownerless-foreign-key-deep-cascade/specs.md`.
- Cyclic foreign-key graphs.
- Concurrent FK action deadlock matrices.
- Crash injection while a cascaded action is in progress.
- Partitioned-table or special-index foreign keys.

## Design

- Add a focused `foreign-key-actions` selector to
  `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates InnoDB parent/child tables while the parent
  keeps an already-open ownerless peer handle, using three foreign-key action
  classes:
  - `ON UPDATE CASCADE ON DELETE CASCADE`;
  - `ON UPDATE CASCADE ON DELETE SET NULL`;
  - `ON DELETE RESTRICT`.
- A child ownerless process updates a parent primary key. The already-open
  parent handle must observe cascaded child key updates and reject inserts
  against the old parent key.
- The child deletes a second parent row. The already-open parent handle must
  observe the cascade delete and `SET NULL` child-row update.
- The child attempts to delete a restricted parent row and verifies MariaDB
  errno 1451. The already-open parent handle must still see the parent and
  restricting child row.
- The test closes peers, validates final state through ownerless read/write
  and ordinary native exclusive reopen, deletes `.shm`, and validates both
  reopen paths again.

## Compatibility Impact

This extends ownerless foreign-key evidence from representative enforcement
and add/drop metadata refresh to representative referential actions. It does
not claim complete foreign-key compatibility; the excluded classes remain
planned until covered or explicitly rejected.

## Directory And Lifecycle Impact

No new files or layout changes. The test exercises native InnoDB foreign-key
actions over tables stored inside the MyLite database directory and validates
the final state through volatile shared-memory rebuild.

## Native Storage Impact

No storage format changes. The slice relies on MariaDB/InnoDB native
foreign-key action execution and MyLite's existing ownerless page visibility,
redo visibility, and dictionary refresh boundaries.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-actions` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- A parent-key update committed by one ownerless process cascades to child rows
  observed by an already-open ownerless peer.
- Inserts referencing the old parent key fail with MariaDB errno 1452 after the
  cascade.
- A parent-row delete committed by one ownerless process deletes `CASCADE`
  children and sets nullable child keys to `NULL` for `SET NULL` children.
- A restricted parent-row delete fails with MariaDB errno 1451 and leaves the
  parent/child rows intact.
- The final action results survive ownerless/native reopen before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- The `ownerless-composite-foreign-key` slice covers a tenant-scoped composite
  foreign-key shape. The `ownerless-foreign-key-deep-cascade` slice covers a
  bounded linear cascade chain. The `ownerless-generated-column-foreign-key`
  slice covers supported stored generated-column FK shapes. The
  `ownerless-cyclic-foreign-key` slice covers a two-table cyclic delete
  cascade and cyclic update rejection. Unsupported generated-column FK
  variants, larger cyclic graph topologies, and crash injection inside
  referential-action execution remain future work.
