# Ownerless Optimistic B-Tree Attribution

## Problem

The production CI run for `e6efccff` split ownerless clustered-low insert
overhead and showed two remaining nonzero buckets: clustered optimistic
B-tree insertion and the row-level clustered insert mini-transaction commit.
The stats-enabled CI sample reported a `0.249 ms/insert`
ownerless-minus-ordinary clustered-low delta, with `0.116 ms/insert` in
clustered optimistic B-tree insertion and `0.129 ms/insert` in the row-level
MTR commit. Duplicate-check, modify-record, instant-root, big-record, and
pessimistic B-tree paths were zero.

Before changing B-tree or page-publication behavior, we need to prove whether
the optimistic B-tree delta is native record sizing and fit checks, lock/undo
work, tuple insertion and page modification, rare page reorganization, adaptive
hash maintenance, or lock inheritance.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc` calls
  `btr_cur_optimistic_insert()` from clustered and secondary insert paths. The
  embedded performance probe creates insert tables with only
  `id INT PRIMARY KEY, value VARCHAR(32)`, so the CI autocommit insert
  attribution is clustered-path evidence.
- `mariadb/storage/innobase/btr/btr0cur.cc` implements
  `btr_cur_optimistic_insert()`. The ordinary successful single-row path:
  computes converted record size, checks big-record/compression/page-fit
  constraints, calls `btr_cur_ins_lock_and_undo()`, inserts the tuple with
  `page_cur_tuple_insert()`, optionally updates adaptive hash state with
  `btr_search_update_hash_on_insert()`, optionally calls
  `lock_update_insert()`, then returns `DB_SUCCESS` without committing the
  caller's mini-transaction.
- The same function also has early `DB_TOO_BIG_RECORD`, `DB_FAIL`, lock wait,
  and corruption/error exits, plus a rare reorganization path when the first
  tuple insert does not fit on an uncompressed page.
- Existing deep counters time total `btr_cur_optimistic_insert()` but do not
  split the successful native subphases or classify result counts.

## Design

Extend the existing stats-gated deep InnoDB counter set with optimistic
B-tree subphase timers for:

- preflight record sizing, big-record conversion, compression, and fit checks,
- `btr_cur_ins_lock_and_undo()`,
- the first `page_cur_tuple_insert()`,
- page reorganization,
- tuple insertion after reorganization,
- adaptive hash update, and
- lock inheritance update.

Add result counters for `DB_SUCCESS`, `DB_FAIL`, lock wait, too-big-record,
other errors, and reorganization attempts. Emit raw
`mylite_perf_*_innodb_deep_row_ins_btr_optimistic_*` values and compact
ordinary, ownerless, and ownerless-minus-ordinary per-insert summaries when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

Keep the existing total optimistic and pessimistic B-tree keys unchanged.

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
MariaDB/InnoDB B-tree insert paths without changing redo, undo, page
publication, checkpoint, B-tree split, lock, or row-insert semantics.

## Build And Performance Impact

The new timers use the existing deep-counter stats gate. The default stats-off
probe does not call the clock for these subphases. Stats-enabled attribution
runs add timestamp reads and summary output outside the default throughput
signal.

## Test And Verification Plan

- Rebuild the production `MinSizeRel` MariaDB embedded archive after editing
  `mariadb/storage/innobase/btr/btr0cur.cc`.
- Build `mylite_embedded_performance_probe` under `php-embedded-prod`.
- Run a reduced stats-enabled production attribution probe and confirm the new
  optimistic B-tree summary keys are present.
- Run a reduced stats-off production probe and confirm the new optimistic
  B-tree attribution keys are absent.
- Run focused ownerless primitive/history tests plus bounded hook/stress
  confidence selectors.
- Run `tools/check-ci-production-builds`.
- Run production format and whitespace checks.

## Local Verification Evidence

Local production verification after rebuilding the `MinSizeRel` MariaDB
embedded archive emitted the new stats-enabled optimistic B-tree summary keys.
The final reduced 50-row sample reported:

- ownerless-minus-ordinary clustered optimistic B-tree total:
  `0.480 ms/insert`,
- `btr_cur_ins_lock_and_undo()`: `0.476 ms/insert`,
- tuple insertion: `0.002 ms/insert`,
- preflight: `0.001 ms/insert`,
- reorganization, reorganization tuple insert, adaptive-hash update, and lock
  update: `0.000 ms/insert`,
- optimistic successes: `1.000` per insert for both ordinary and ownerless,
  and
- `DB_FAIL`, lock wait, too-big-record, other-error, and reorganization
  attempt counts: `0.000` per insert.

The separate stats-off 500-row production probe emitted no
`clustered_btree_optimistic_*` attribution keys and reported ownerless
autocommit at `1067.84 ops/s` versus ordinary autocommit at `2037.75 ops/s`,
a `0.5240` ratio.

## Acceptance Criteria

- The deep-counter enum stays aligned between MariaDB and the C performance
  probe mirror.
- Stats-enabled production attribution emits optimistic B-tree preflight,
  lock/undo, tuple insert, reorg, AHI update, lock update, and result-count
  summaries for ordinary and ownerless autocommit inserts plus deltas.
- Stats-off production throughput output remains unchanged.
- Docs continue to avoid claiming ownerless concurrency completion.

## Risks And Unresolved Questions

- Stats-enabled timing may perturb the short reduced attribution run; use it to
  identify phase shape, not as the default throughput signal.
- If tuple insertion dominates, the next step must distinguish native page
  modification from ownerless page-write hooks before optimizing.
- If lock/undo dominates, the next step belongs near transaction/undo reuse or
  lock inheritance, not B-tree fit checks.
- Row-level MTR commit remains a separate nonzero bucket and likely needs
  context-tagged MTR attribution before optimization.
