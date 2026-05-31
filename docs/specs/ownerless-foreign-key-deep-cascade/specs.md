# Ownerless Foreign-Key Deep Cascade

## Problem

Ownerless foreign-key action coverage proves representative single-hop
`CASCADE`, `SET NULL`, and `RESTRICT` behavior. It still does not prove that a
single parent-row change can recursively cascade through multiple InnoDB
foreign-key edges while an already-open ownerless peer observes every committed
level and the final state survives ownerless/native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc` implements cascaded foreign-key
  actions in `row_ins_foreign_check_on_constraint()`. It creates a child update
  node for `DELETE_CASCADE`, `DELETE_SET_NULL`, `UPDATE_CASCADE`, and
  `UPDATE_SET_NULL`, and reports row-is-referenced errors when no action flag
  allows the parent change.
- The same InnoDB path counts ancestor update/delete nodes with
  `row_ins_cascade_n_ancestors()` and rejects cascades at
  `FK_MAX_CASCADE_DEL` (`15` in `dict0mem.h`), so a bounded four-table chain is
  inside MariaDB's native cascade depth limit while still exercising recursive
  action execution.
- InnoDB blocks cyclic cascaded updates of the same table via
  `row_ins_cascade_ancestor_updates_table()`. This slice deliberately uses a
  linear table chain rather than a cyclic graph.
- MyLite ownerless commits publish dirty pages into the page-version WAL before
  page-visible LSN publication, and ownerless statement refresh makes peer
  committed pages visible to already-open handles.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for a four-table InnoDB chain:
  `root -> level1 -> level2 -> level3`.
- One root primary-key update that cascades the key through all dependent
  levels.
- One root delete that cascades through all dependent levels and removes the
  matching dependent rows.
- Already-open ownerless peer visibility after each committed cascade boundary.
- Missing-parent enforcement through the deepest table after the cascade update.
- Final ownerless and native exclusive reopen checks before and after forced
  `.shm` rebuild.

Out of scope:

- Cyclic foreign-key graph behavior; the bounded two-table shape is covered
  separately by `docs/specs/ownerless-cyclic-foreign-key/specs.md`.
- Supported stored generated-column foreign keys; that shape is covered
  separately by
  `docs/specs/ownerless-generated-column-foreign-key/specs.md`.
- Crash/error injection while a deep cascaded action is in progress.
- Cascades at or beyond MariaDB's native maximum cascade depth.
- Partitioned-table or special-index foreign keys.

## Design

- Add a focused `foreign-key-deep-cascade` selector to
  `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates four InnoDB tables in `app`. Each dependent
  table uses its primary key as a foreign key to the previous level with
  `ON UPDATE CASCADE ON DELETE CASCADE`.
- The parent keeps an already-open ownerless peer, verifies the three cascade
  constraints, and releases the child to update `root.id = 1` to `10`.
- After the child commits the update, the peer verifies `id = 10` and absence
  of `id = 1` at every level, then verifies inserting the deepest level without
  a matching parent fails with MariaDB errno 1452.
- The peer inserts a new valid four-level chain for `id = 3`.
- The child deletes `root.id = 2`. The peer verifies `id = 2` is absent at
  every level while the cascaded `id = 10` rows and peer-inserted `id = 3`
  rows remain.
- Final state is verified through ownerless read/write and ordinary native
  exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for recursive InnoDB foreign-key action
execution across multiple tables. It does not claim complete foreign-key graph
coverage, larger cyclic graph topology support, unsupported generated-column
foreign-key variants, or crash/error recovery inside the cascaded action.

## Directory And Lifecycle Impact

No new files or layout changes. The slice uses native InnoDB files inside the
MyLite database directory and validates final state after volatile
shared-memory rebuild.

## Native Storage Impact

No storage-format changes. The test relies on MariaDB/InnoDB native recursive
cascade execution and MyLite's existing ownerless page visibility, redo
visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-deep-cascade` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes three `CASCADE` constraints across
  the four-table chain.
- A root primary-key update committed by one ownerless process cascades to all
  dependent levels observed by the already-open peer.
- A deepest-level insert referencing the old key fails with MariaDB errno 1452
  after the cascade update.
- A root delete committed by one ownerless process removes the matching rows
  at every dependent level while unrelated rows remain.
- The final rows and constraint metadata survive ownerless/native reopen before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one bounded linear deep-cascade shape. Larger cyclic graph
  topologies, unsupported generated-column FK variants, and crash/error
  injection during cascaded actions remain follow-up DDL/recovery work.
