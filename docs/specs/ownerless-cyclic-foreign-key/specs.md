# Ownerless Cyclic Foreign Key

## Problem

Ownerless foreign-key coverage now proves ordinary referential actions,
composite keys, deep linear cascades, supported stored generated-column foreign
keys, and foreign-key rename refresh. It still does not prove cyclic
foreign-key graphs, where InnoDB must coordinate dictionary metadata,
referential-action recursion, row locks, and peer-visible page updates across
more than one table in a cycle.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc` implements
  `row_ins_foreign_check_on_constraint()` for delete/update referential
  actions. It reports `DB_ROW_IS_REFERENCED` when no matching action exists,
  creates a cascade node for `DELETE_CASCADE` or update actions, and enforces
  `FK_MAX_CASCADE_DEL` for recursive chains.
- The same function explicitly rejects cyclic cascaded updates through
  `row_ins_cascade_ancestor_updates_table()` because an update cycle could see
  parent-table indexes in an inconsistent state. The source comment states that
  cyclic delete is allowed but cyclic update is not.
- `row_ins_cascade_n_ancestors()` bounds recursive delete/update chains, so a
  cyclic delete graph must terminate through row deletion rather than loop
  forever.
- `mariadb/mysql-test/suite/innodb/t/add_constraint.test` creates two InnoDB
  tables, adds a foreign key from `t2` to `t1`, then adds a reverse foreign key
  from `t1` to `t2`; the corresponding result accepts the mutual constraints.
- Existing ownerless FK action and deep-cascade selectors already prove
  non-cyclic `ON DELETE CASCADE`, `ON UPDATE CASCADE`, and missing-parent /
  restricted-parent errno behavior. This slice adds the graph-cycle boundary.

## Scope And Non-Goals

In scope:

- A two-table InnoDB cycle where both constraints use
  `ON UPDATE RESTRICT ON DELETE CASCADE`.
- Cross-process ownerless SQL coverage for creating the cycle, inserting
  mutual references, peer-visible constraint metadata, missing-parent
  enforcement, restricted parent-key update failures, cyclic delete cascade,
  valid cycle insertion after the cascade boundary, and ownerless/native reopen
  before and after forced `.shm` rebuild.
- A bounded negative check for cyclic `ON UPDATE CASCADE` using a separate
  two-table cycle that would update the initiating table again, expecting
  MariaDB errno 1451 instead of claiming cyclic update support.

Out of scope:

- Multi-table cycles larger than two tables.
- Cyclic `SET NULL` matrices.
- Crash/error injection while a cyclic referential action is in progress.
- Cascades at or beyond MariaDB's native maximum cascade depth.
- Partitioned-table or special-index foreign keys.

## Design

- Add a focused `cyclic-foreign-key` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates `ownerless_fk_cycle_a` and
  `ownerless_fk_cycle_b`, then adds mutual foreign keys with
  `ON UPDATE RESTRICT ON DELETE CASCADE`.
- The child inserts a valid two-row cycle by first inserting one nullable edge,
  inserting the reciprocal row, then updating the first row to complete the
  cycle.
- The already-open peer verifies row counts, edge values, and both
  `REFERENTIAL_CONSTRAINTS` rows.
- The child verifies parent-key updates on either side of the cycle fail with
  MariaDB errno 1451.
- The peer verifies missing-parent inserts/updates fail with MariaDB errno
  1452.
- The child deletes one side of the cycle and commits; the peer verifies both
  tables are empty, proving the cyclic delete cascade reached the reciprocal
  row without leaving an orphan.
- The peer inserts a new valid cycle after the cascade boundary and commits.
- A separate two-table cycle with `ON UPDATE CASCADE ON DELETE CASCADE` is used
  only to assert that a cyclic cascaded update returns errno 1451, in line with
  InnoDB's source-level restriction.
- Final state and constraint metadata are verified through ownerless read/write
  and ordinary native exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for MariaDB-supported cyclic delete
cascades and explicitly documents the MariaDB-native cyclic update rejection.
It does not claim arbitrary cyclic graph coverage, cyclic `SET NULL`, crash
recovery during referential actions, or behavior beyond MariaDB's native
cascade-depth limit.

## Directory And Lifecycle Impact

No new files or layout changes. The slice uses ordinary native InnoDB table
files under the MyLite database directory and verifies final state after
volatile shared-memory rebuild.

## Native Storage Impact

No storage-format changes. The test relies on InnoDB native dictionary
metadata, mutual foreign-key lookup, recursive delete-cascade execution,
restricted update diagnostics, and MyLite's existing ownerless page visibility,
redo visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `cyclic-foreign-key` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes both constraints in a mutual
  two-table FK cycle.
- Parent-key updates on either side of the cycle fail with MariaDB errno 1451.
- Missing-parent inserts or edge updates fail with MariaDB errno 1452.
- Deleting one side of the cycle cascades through the reciprocal table and
  leaves no rows in either table.
- A new valid two-row cycle can be inserted after the cascade boundary.
- A cyclic cascaded update shape is rejected with MariaDB errno 1451.
- Final rows and constraint metadata survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one two-table cycle and one cyclic-update rejection. Larger graph
  topologies, cyclic `SET NULL`, crash/error injection during FK execution,
  and external randomized FK graph stress remain follow-up work.
