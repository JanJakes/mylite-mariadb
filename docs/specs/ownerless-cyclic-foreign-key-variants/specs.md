# Ownerless Cyclic Foreign Key Variants

## Problem

The `ownerless-cyclic-foreign-key` slice proves a two-table cyclic
`ON DELETE CASCADE` graph and MariaDB's cyclic update rejection. Remaining
cyclic FK evidence is still too narrow: it does not cover a larger cycle
topology or a cyclic `ON DELETE SET NULL` action, both of which exercise
different InnoDB referential-action paths while ownerless peers are open.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc` dispatches delete actions in
  `row_ins_foreign_check_on_constraint()` by building a cascade node when a
  referenced row has child rows and the constraint has `DELETE_CASCADE` or
  `DELETE_SET_NULL`.
- For `DELETE_SET_NULL`, the same function builds an update vector that sets
  the foreign-key columns in the child row to SQL `NULL`.
- `row_ins_cascade_n_ancestors()` enforces `FK_MAX_CASCADE_DEL` for recursive
  delete/update chains. A three-table cycle remains well below that native
  bound while still proving more than the two-table case.
- `mariadb/mysql-test/suite/innodb/t/add_constraint.test` shows MariaDB accepts
  mutual InnoDB foreign keys added after table creation, establishing that
  cyclic dictionary metadata is a supported native shape.
- Existing ownerless FK slices already cover ordinary `ON DELETE SET NULL`,
  two-table cyclic `ON DELETE CASCADE`, deep linear cascades, and cyclic update
  rejection. This slice combines the remaining cyclic action/topology variants.

## Scope And Non-Goals

In scope:

- A three-table InnoDB cycle using `ON UPDATE RESTRICT ON DELETE CASCADE`.
- A two-table InnoDB cycle using `ON UPDATE RESTRICT ON DELETE SET NULL`.
- Cross-process ownerless SQL coverage for creating both cyclic shapes,
  observing their metadata through an already-open peer, deleting through the
  cascade cycle, deleting each side of the `SET NULL` cycle, inserting valid
  rows after those action boundaries, and ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Cyclic `ON UPDATE CASCADE`, already covered as a MariaDB-native rejection by
  `docs/specs/ownerless-cyclic-foreign-key/specs.md`.
- Cascades at or beyond MariaDB's native maximum cascade depth.
- Crash/error injection while a cyclic referential action is in progress.
- Randomized FK graph generation and external MariaDB/RQG oracle stress.
- Partitioned-table or special-index foreign keys.

## Design

- Add a focused `cyclic-foreign-key-variants` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates:
  - `ownerless_fk_cycle3_a/b/c`, a three-table cycle where deleting `a` must
    cascade through `b` and `c`.
  - `ownerless_fk_cycle_null_a/b`, a two-table cycle where deleting either
    side must set the reciprocal edge to `NULL` instead of deleting the child.
- The already-open peer verifies all five constraints in
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS` and checks the initial cyclic
  edge values.
- The child deletes one row from the three-table cascade cycle and one parent
  row from the `SET NULL` cycle; the peer verifies the cascade cycle is empty
  and the remaining `SET NULL` row has a null edge.
- The peer inserts a new three-table cycle and a second `SET NULL` cycle. The
  child deletes the opposite side of the second `SET NULL` cycle; the peer
  verifies the reciprocal row remains with a null edge.
- The peer inserts final valid rows in both cyclic shapes.
- Final rows and metadata are verified through ownerless read/write and
  ordinary native exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for a larger cyclic cascade topology and
cyclic `SET NULL` behavior. It still does not claim arbitrary FK graph
generation, cyclic update support, crash recovery during referential actions,
or behavior beyond MariaDB's native cascade-depth limit.

## Directory And Lifecycle Impact

No new files or layout changes. The slice uses native InnoDB files inside the
MyLite database directory and verifies final state after volatile shared-memory
rebuild.

## Native Storage Impact

No storage-format changes. The test relies on InnoDB native dictionary
metadata, recursive delete-cascade execution, set-null referential actions, and
MyLite's existing ownerless page visibility, redo visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `cyclic-foreign-key-variants` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the three-table cascade cycle and
  two-table set-null cycle constraints.
- Deleting one side of the three-table cycle removes all rows in the cycle.
- Deleting either side of the `SET NULL` cycle preserves the reciprocal row and
  sets its edge to SQL `NULL`.
- Valid cyclic rows can be inserted after both action boundaries.
- Final rows and constraint metadata survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves two additional cyclic variants. Crash/error injection during FK
  execution and external randomized FK graph stress remain follow-up recovery
  and compatibility work.
