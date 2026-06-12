# Ownerless Visible Anchor Profile

## Problem

The ownerless autocommit write path still has a large production performance
gap after compact sparse page-log encoding. The latest CI sample showed the
remaining hot path concentrated in page-visible publication and native InnoDB
mini-transaction work:

- page-visible hook total: about `0.475` to `0.488 ms` per insert,
- page-log visibility sync: about `0.378 ms` per insert in one CI sample,
- durable visible checkpoint update: about `0.097 ms` per insert in the same
  sample,
- row/undo mini-transaction work still adds separate ownerless deltas.

The current counters show total page-log sync and checkpoint spans, but they do
not separate byte-range lock time, header validation, read/write time, and
`fdatasync` time. That makes the next optimization choice ambiguous: the
right answer may be sync batching, lock contention reduction, checkpoint write
coalescing, or a native redo/checkpoint proof. This slice adds that missing
attribution without changing the durability contract.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` commits mini-transactions at page
  LSN boundaries; ownerless visibility must continue to respect those page
  publication boundaries rather than SQL text alone.
- `packages/libmylite/src/database.cc::ownerless_innodb_pages_visible_hook()`
  currently publishes a visible LSN by first syncing the ownerless page-version
  WAL, then publishing visible redo state in shared memory, then persisting the
  latest/visible LSN pair in `mylite-concurrency.ckpt`.
- `packages/libmylite/src/database.cc::update_concurrency_checkpoint_lsn()`
  locks the checkpoint byte range, reads the existing latest/visible LSNs,
  writes the max-advanced pair, and uses `fdatasync()`/`fsync()` when called
  with `durable=true`.
- `packages/libmylite/src/ownerless_page_log.cc::sync_at_common()` validates
  the existing page-log header under the append/snapshot lock byte and then
  uses `fdatasync()`/`fsync()` to make appended page-version records durable
  before shared visibility is published.
- The crash tests in
  `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already kill
  writers before and after the visible checkpoint. They prove the current order
  is observable and must not be weakened by a profiling-only slice.

## Design

Add timing counters for the two durable anchors that make a committed page
version visible across processes:

1. Ownerless page-log sync subphases:
   - calls,
   - total elapsed time,
   - snapshot-lock wait/hold acquisition time,
   - existing-header validation time,
   - data-sync time.
2. Ownerless checkpoint update subphases:
   - calls,
   - total elapsed time,
   - checkpoint-lock acquisition time,
   - current-LSN read time,
   - max-advanced payload write time,
   - durable data-sync time.

The counters are enabled only by the existing performance probe path. They are
first-party internal diagnostics, like the current page-log append and database
performance counters. Production behavior, file format, SQL behavior, locking
order, and sync order are unchanged.

The embedded performance probe will emit raw totals and per-ownerless-insert
summary values so CI can show whether visibility cost is mostly filesystem
sync, lock acquisition, or local read/write overhead.

## Compatibility Impact

No SQL, C API, PHP API, mysqli behavior, storage-engine semantics, wire
protocol behavior, or directory layout changes. This slice only adds internal
performance counters and CI-visible output.

## Directory And Lifecycle Impact

No files are added or removed. `mylite-concurrency.wal` is still synced before
the `.shm` page-visible LSN is published, and `mylite-concurrency.ckpt` is
still durably updated after page-visible publication.

## Native Storage Impact

Native InnoDB files, redo, and checkpoint state are unchanged. The slice does
not claim native redo/checkpoint reconciliation is complete and does not remove
the existing page-visible checkpoint anchor.

## Build And Performance Impact

Stats-disabled builds add no new hot-path timing calls. Stats-enabled
diagnostic runs add a few steady-clock samples around existing syscalls, which
is acceptable for attribution runs and already isolated from ordinary PHPUnit
and production correctness steps.

The expected output is diagnostic rather than an immediate speedup. If the new
numbers show durable sync dominating, a later optimization must still prove
that batching, coalescing, or deferring a sync preserves crash recovery across
forced shared-memory rebuilds.

A local reduced production attribution run with 100 insert iterations reported
ownerless autocommit at `453.68` ops/s versus ordinary autocommit at
`1270.01` ops/s. In that sample, the page-visible hook itself was only
`0.012 ms` per insert: `0.006 ms` in page-log sync and `0.006 ms` in visible
checkpoint publication. The lower-level page-log sync split reported one call
per insert with `0.005 ms` total, including `0.001 ms` in data sync. The
broader checkpoint update primitive ran four times per insert with
`0.137 ms` total, dominated by `0.113 ms` in current-LSN reads and effectively
zero durable sync time. The same run still showed larger native
mini-transaction costs, including about `0.235 ms` ownerless-minus-ordinary
clustered low MTR commit time and `0.443 ms` ownerless-minus-ordinary
`trx_undo_report()` MTR commit time. This sample is local and noisy, but it
keeps the next optimization target on checkpoint update call/read volume and
native MTR work rather than blindly weakening visible WAL sync.

## Tests And Verification Plan

- Extend ownerless primitive coverage so page-log sync counters report calls,
  total time, header validation, and data-sync time for initialized sync.
- Extend the embedded performance probe enum mirrors and raw output.
- Add per-insert ownerless autocommit summary lines for page-log sync and
  checkpoint subphases.
- Run focused ownerless primitive coverage, visible-publish and visible-
  checkpoint crash coverage, the reduced production performance probe with
  stats enabled, production build guards, format check, and diff whitespace
  checks.

## Acceptance Criteria

- Existing ownerless page-log sync behavior and initialized-header validation
  remain unchanged.
- Existing visible-publish crash and visible-checkpoint crash tests pass.
- Production stats-enabled performance output identifies page-log sync and
  checkpoint lock/read/write/sync costs separately.
- Docs make clear this is an attribution slice, not a durability relaxation.

## Risks And Unresolved Questions

- Timing counters can perturb very small samples. The probe should use the
  values as directionally useful attribution and compare them with stats-off
  throughput separately.
- The next optimization may require stronger native checkpoint proof before it
  can safely reduce per-commit sync frequency.
- Broader ownerless gaps remain: SQL-level table-lock fault injection,
  additional DDL/file lifecycle crash recovery, broader active-reader pressure
  policy, broader DDL/dictionary/allocation classes, and longer external
  MariaDB/RQG stress.
