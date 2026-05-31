# Ownerless Foreign-Key Graph Stress

## Problem

Ownerless foreign-key coverage now includes representative single-hop actions,
deep cascade chains, composite keys, cyclic graphs, generated-column policy, and
rename refresh. The remaining foreign-key gap calls out randomized graph stress
and external oracle coverage. Full external MariaDB/RQG stress is still a
larger harness project, but the ownerless stress preset can add a bounded
deterministic graph stress case that repeatedly mutates one shared InnoDB
foreign-key graph from multiple ownerless processes.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/dict0mem.h:1518-1557` defines
  `dict_foreign_t`, including referenced/foreign table pointers, foreign and
  referenced indexes, and type bits for actions.
- `mariadb/storage/innobase/row/row0ins.cc:1045-1056` returns
  `DB_ROW_IS_REFERENCED` when a parent update/delete has no matching cascade or
  set-null action.
- `row0ins.cc:1069-1081` chooses cascaded delete versus child-row update
  execution and sizes the update vector for the foreign key.
- `row0ins.cc:1178-1194` locks the affected child row before executing the
  referential action.
- `row0ins.cc:1217-1246` builds the `SET NULL` update vector, and
  `row0ins.cc:1268-1309` builds the `UPDATE CASCADE` vector or rejects an
  impossible cascade.
- `mariadb/storage/innobase/row/row0mysql.cc:1938-1998` executes cascaded
  delete/set-null/update nodes and tracks cascade depth.

## Scope And Non-Goals

In scope:

- Add an opt-in `fk-graph-stress` selector to
  `mylite_ownerless_cross_process_sql_test`.
- Add the selector to the `ownerless-stress` preset.
- Run multiple ownerless writer processes over one shared FK graph with:
  - `ON UPDATE CASCADE ON DELETE CASCADE`,
  - `ON UPDATE CASCADE ON DELETE SET NULL`,
  - `ON UPDATE RESTRICT ON DELETE RESTRICT`,
  - missing-parent insert failures,
  - restricted parent-delete failures.
- Verify deterministic aggregate oracles through ownerless and native exclusive
  reopen before and after forced `.shm` rebuild.

Out of scope:

- External MariaDB/RQG/SQLancer orchestration.
- Random SQL generation.
- Crash injection inside referential-action execution.
- New production lock, recovery, or file-lifecycle behavior.

## Design

Create a parent table and three child tables:

- `ownerless_fk_graph_cascade_child` references the parent with
  `ON UPDATE CASCADE ON DELETE CASCADE`.
- `ownerless_fk_graph_setnull_child` references the parent with
  `ON UPDATE CASCADE ON DELETE SET NULL`.
- `ownerless_fk_graph_restrict_child` references the parent with
  `ON UPDATE RESTRICT ON DELETE RESTRICT`.

Each worker owns disjoint parent and child rows, so expected totals are
deterministic while the workers still contend on the same native tables,
dictionary metadata, foreign-key structures, redo, page-version WAL, and
ownerless lock/refresh paths. Every round updates parent primary keys for the
cascade and set-null edges, updates values and version counters in all rows,
and verifies representative missing-parent and restricted-delete failures.
Because native FK parent-key updates can serialize through InnoDB table and
referential-action locks, each worker retries the whole round after MariaDB
lock-wait timeout or deadlock errors (1205/1213) and only advances its
deterministic local oracle after commit. At the end each worker deletes its
set-null parent so final assertions must observe the nullable child foreign key
set to `NULL`.

The final oracle checks row counts, version sums, child reference sums,
`NULL` counts, referential-constraint rules, and aggregate values through:

- `MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW`,
- ordinary `MYLITE_OPEN_READWRITE`,
- both modes again after removing volatile `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

No SQL behavior changes. The slice adds deterministic ownerless stress evidence
for a multi-edge InnoDB foreign-key graph, narrowing the gap between focused FK
selectors and future external randomized FK graph stress.

## Directory And Lifecycle Impact

No directory layout changes. The test uses native InnoDB tables inside the
MyLite database directory and verifies final state after normal close and forced
shared-memory rebuild.

## Native Storage Impact

No native format changes. The test relies on MariaDB/InnoDB foreign-key action
execution and MyLite's existing ownerless page visibility, redo visibility,
lock coordination, and reopen paths.

## Binary Size And Dependencies

No production binary-size, dependency, or license impact. The slice adds test
code only.

## Test Plan

- Configure and build `ownerless-stress`.
- Run focused `fk-graph-stress` in `ownerless-stress`.
- Build and run focused `fk-graph-stress` in `ownerless-test-hooks`.
- Run embedded and hook ownerless cross-process SQL CTests.
- Run the full `ownerless-stress` preset.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Concurrent ownerless FK graph workers complete with bounded 1205/1213 retry
  and no aggregate mismatch.
- Cascaded parent primary-key updates are visible in child rows.
- Set-null parent deletes leave nullable child references `NULL`.
- Restricted parent deletes fail with errno 1451 and leave rows intact.
- Missing-parent inserts fail with errno 1452.
- Final row, reference, constraint, and value oracles survive ownerless/native
  reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is deterministic in-process stress, not external MariaDB/RQG stress.
- Crash injection inside referential-action execution remains separate recovery
  work.
