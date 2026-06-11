# Ownerless Clustered Insert Low Attribution

## Problem

The production row-insert attribution slice showed that ownerless autocommit
insert overhead is concentrated in the clustered row graph. The latest
stats-enabled CI sample reports ownerless-minus-ordinary deltas under
`row_insert_step`, `row_ins_index_entry`, and `row_ins_clust_low`, with
secondary-index and clustered pessimistic B-tree paths at zero. The compact
summary already times `btr_cur_optimistic_insert()`, but about half of the
clustered-low delta remains outside that existing optimistic insert counter.

Before changing page publication or B-tree behavior, we need to prove whether
the remaining clustered-low cost is index search, duplicate checking,
mini-transaction commit/page publication, or a rare fallback path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc` implements
  `row_ins_clust_index_entry_low()`. For ordinary single-row clustered inserts
  it starts a mini-transaction, calls `btr_pcur_open(..., PAGE_CUR_LE, ...)`,
  checks duplicate-key state when needed, calls `btr_cur_optimistic_insert()`,
  then commits the mini-transaction.
- The same function also has less common paths for modifying an existing
  delete-marked record, updating instant-root metadata, writing externally
  stored big records, and error/bulk exits.
- Existing MyLite deep counters already time total
  `row_ins_clust_index_entry_low()` and the `btr_cur_optimistic_insert()` or
  pessimistic insert calls, but they do not time the clustered-low search,
  duplicate check, final row-level `mtr.commit()`, or rare post-insert paths.
- `mariadb/storage/innobase/btr/btr0cur.cc` implements
  `btr_cur_optimistic_insert()`. That function checks fit/reorganize
  preconditions, writes undo/locks through `btr_cur_ins_lock_and_undo()`,
  inserts the tuple with `page_cur_tuple_insert()`, updates adaptive hash and
  record locks when needed, and returns without committing the caller's
  mini-transaction.
- Ownerless page-version publication is tied to mini-transaction commit paths
  in `mariadb/storage/innobase/mtr/mtr0mtr.cc`, so separating
  `btr_cur_optimistic_insert()` from the surrounding row-level `mtr.commit()`
  is necessary before choosing a safe optimization.

## Design

Extend the existing stats-gated deep InnoDB counter set with
`row_ins_clust_low_*` subphase timers for:

- `btr_pcur_open()` search,
- clustered duplicate checking,
- modify-record fallback,
- instant-root update,
- row-level `mtr.commit()`, and
- big-record follow-up.

Emit raw `mylite_perf_*_innodb_deep_row_ins_clust_low_*` values and compact
ordinary, ownerless, and ownerless-minus-ordinary per-insert summaries when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

Keep the existing total clustered-low, clustered optimistic B-tree, and
clustered pessimistic B-tree keys unchanged.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, locking, metadata, native storage format, or
directory-lifecycle behavior changes. The slice adds opt-in performance
instrumentation only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The embedded performance probe continues
to create and remove temporary MyLite directories.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage behavior changes. The new counters observe existing
MariaDB/InnoDB B-tree and mini-transaction paths without changing redo, undo,
page publication, checkpoint, or row-insert semantics.

## Build And Performance Impact

The new timers use the existing deep-counter stats gate. The default stats-off
probe does not call the clock for these subphases. Stats-enabled attribution
runs add a small number of timestamp reads and `printf()` calls outside the
default throughput signal.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` under `php-embedded-prod`.
- Run a reduced stats-enabled production attribution probe and confirm the new
  clustered-low summary keys are present.
- Run a reduced stats-off production probe and confirm the new clustered-low
  attribution keys are absent.
- Run focused ownerless primitive/history tests.
- Run `tools/check-ci-production-builds`.
- Run production format and whitespace checks.

## Local Verification Evidence

Local production verification after rebuilding the `MinSizeRel` MariaDB
embedded archive emitted the new stats-enabled clustered-low summary keys.
Short 50-row attribution samples were noisy in absolute magnitude, but the
final rerun kept the same phase shape and reported:

- ownerless-minus-ordinary clustered-low total: `0.472 ms/insert`,
- clustered optimistic B-tree: `0.378 ms/insert`,
- row-level MTR commit: `0.084 ms/insert`,
- `btr_pcur_open()` search: `0.010 ms/insert`, and
- duplicate-check, modify-record, instant-root, big-record, and pessimistic
  B-tree deltas: `0.000 ms/insert`.

The separate stats-off 100-row production probe emitted no
`row_ins_clust_low_*` attribution keys, preserving the default throughput
signal.

## Acceptance Criteria

- The deep-counter enum stays aligned between MariaDB and the C performance
  probe mirror.
- Stats-enabled production attribution emits clustered-low search,
  duplicate-check, modify-record, instant-root, MTR-commit, and big-record
  summaries for ordinary and ownerless autocommit inserts plus deltas.
- Stats-off production throughput output remains unchanged.
- Docs continue to avoid claiming ownerless concurrency completion.

## Risks And Unresolved Questions

- If row-level `mtr.commit()` dominates, the next optimization likely needs a
  safer page-publication batching or native redo/checkpoint proof rather than a
  row-insert algorithm change.
- If `btr_pcur_open()` dominates, the next slice should inspect latch/search
  behavior before assuming page WAL is responsible.
- Broader correctness gaps remain: DDL/file lifecycle recovery, redo/checkpoint
  reconciliation, active-reader pressure policy, and longer randomized external
  MariaDB/RQG stress.
