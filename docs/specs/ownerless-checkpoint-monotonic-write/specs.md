# Ownerless Checkpoint Monotonic Write

## Problem

The ownerless commit path updates `concurrency/mylite-concurrency.ckpt`
several times per autocommit insert. The current updater takes the checkpoint
byte-range lock, reads the existing latest/page-visible LSN pair from the file,
merges it with the caller's LSNs, writes the merged pair, and optionally syncs
the file.

The read is redundant for hook paths that have already published into the
directory-owned redo state, but removing it is only safe if the writer still
cannot move either checkpoint LSN backward. A raw latest-LSN write is especially
easy to get wrong because its visible input is zero even when the shared visible
LSN and the checkpoint file may already be nonzero.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::Command::execute()` commits
  mini-transactions, sets page LSNs, and drives the page-publication hooks that
  make physical page images visible to MyLite peers.
- `mariadb/storage/innobase/trx/trx0trx.cc` drives transaction commit and the
  MyLite redo/visibility hooks used by ownerless commit publication.
- `packages/libmylite/src/ownerless_redo_state.cc::
  mylite_ownerless_redo_state_leave()` publishes a new raw latest LSN with a
  fetch-max operation and reports only the LSN that actually advanced the
  shared latest field.
- `packages/libmylite/src/ownerless_redo_state.cc::
  mylite_ownerless_redo_state_publish_visible()` caps the requested visible LSN
  at the shared written LSN, fetch-maxes both latest and visible shared fields,
  updates durable visible state, and returns the published shared pair.
- `packages/libmylite/src/database.cc::ownerless_innodb_redo_leave_hook()`
  persists raw latest checkpoint progress with `visible_lsn == 0`.
- `packages/libmylite/src/database.cc::ownerless_innodb_pages_visible_hook()`
  syncs the ownerless page-version WAL, publishes the shared page-visible LSN,
  and durably updates `.ckpt`.
- `packages/libmylite/src/database.cc::update_concurrency_checkpoint_lsn()`
  must keep its file read for non-hook callers such as startup native baseline
  seeding and no-live native checkpoint promotion, because those paths can use
  native checkpoint evidence rather than a just-published redo-state pair.

## Design

Add an internal checkpoint update helper for redo-state-backed hook paths:

```c++
bool update_concurrency_checkpoint_lsn_from_redo_state(
    int checkpoint_fd,
    void *redo_state,
    size_t redo_state_size,
    uint64_t latest_lsn,
    uint64_t visible_lsn,
    bool durable);
```

The helper:

- keeps the existing checkpoint byte-range lock,
- snapshots shared redo state only after holding that lock,
- maxes the caller's latest and visible inputs with the locked snapshot's
  latest, visible, and durable-visible fields,
- writes the normalized pair to the same `.ckpt` offsets as before,
- keeps the existing durable sync behavior when `durable` is true, and
- records a performance counter for elided checkpoint-file reads.

Reading shared redo state after taking the checkpoint lock is required. If a
peer publishes a higher shared LSN and writes `.ckpt` first, the later writer's
locked shared-state snapshot will see the higher LSN and preserve it. If the
peer publishes after the snapshot, that peer's later checkpoint write will
advance `.ckpt` after this writer releases the lock.

The existing file-read updater remains unchanged for direct native checkpoint
paths. This slice does not make checkpoint writes lazy, does not change the
`.ckpt` record format, and does not claim a torn-write-safe durable checkpoint
record.

## Scope And Non-Goals

In scope:

- ownerless redo/latest and page-visible hook checkpoint updates,
- preservation of monotonic latest and visible LSNs without rereading `.ckpt`,
- performance-probe output for elided checkpoint-file reads, and
- crash-hook and performance verification for the same commit ordering.

Out of scope:

- changing `mylite-concurrency.ckpt` format,
- adding generation/checksum/double-buffered checkpoint records,
- deferring, batching, or dropping durable checkpoint syncs,
- replacing the page-version WAL sync with native redo proof, and
- claiming ownerless autocommit insert throughput is close to trunk/native.

## Compatibility Impact

No SQL behavior, C API behavior, PHP/mysqli behavior, wire-protocol behavior,
or public directory layout changes. The ownerless hooks still publish the same
durable checkpoint LSNs in the same order; they avoid only a redundant file
read when shared redo state already provides the monotonic boundary.

## Directory And Lifecycle Impact

No files are added. The checkpoint file remains
`concurrency/mylite-concurrency.ckpt`; the redo state remains in
`concurrency/mylite-concurrency.shm`. The helper relies on the existing startup
path that seeds shared redo state from `.ckpt` and on the no-live reclaim path
that reseeds shared redo state after advancing the durable checkpoint-visible
LSN.

## Native Storage Impact

Native InnoDB files, redo, page flushing, and native checkpoint behavior are
unchanged. The helper is only a MyLite-owned checkpoint metadata write
optimization around already-published ownerless redo state.

## Build And Performance Impact

No dependency or build-profile changes. The expected performance impact is a
small reduction in ownerless autocommit checkpoint-update time by eliminating
the `pread()` from hook-driven checkpoint updates. The larger remaining cost is
still durable page-version/metadata publication and native mini-transaction
page publication.

## Test And Verification Plan

- Run focused ownerless primitive redo-state coverage.
- Run visible-publish and visible-checkpoint crash-hook coverage to prove the
  durable ordering still holds. This is covered by the hook-build
  `visible-publish-crash` and `visible-checkpoint-crash` direct cases.
- Run raw redo latest before/after checkpoint crash-hook coverage to prove raw
  latest checkpoint progress remains recoverable. This is covered by the
  hook-build `redo-latest-crash` and `redo-latest-checkpoint-crash` direct
  cases.
- Run reduced production `embedded_performance_probe` with ownerless page
  publish stats enabled and verify checkpoint file-read time drops while the
  new elided-read counter is nonzero.
- Run ownerless SQL/performance focused selectors that exercise the commit path.
- Run format and whitespace checks.

## Acceptance Criteria

- Redo hook checkpoint writes do not call the checkpoint-file read path.
- Latest and visible checkpoint LSNs remain monotonic across raw-latest and
  page-visible hook writes.
- Existing startup/no-live direct checkpoint updates still merge against the
  file payload.
- Crash-hook coverage for redo latest and visible publish/checkpoint passes.
- Performance logs expose the elided checkpoint-file read count.

## Follow-Up Evidence

A 2026-06-14 hook-build audit reran the direct checkpoint publication crash
cases after later ownerless work:

```sh
cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test
for case_name in \
  visible-publish-crash \
  visible-checkpoint-crash \
  redo-latest-crash \
  redo-latest-checkpoint-crash; do
  rm -rf /tmp/mylite-ownerless-sql.* /tmp/mylite-ownerless-*
  build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test "${case_name}"
done
```

All four direct cases passed. This confirms the raw-latest and page-visible
crash-hook coverage described in the ownerless cross-process concurrency spec
is present and still runnable from the unsafe hook build.

## Risks And Unresolved Questions

- This slice removes only a small syscall cost and does not address the larger
  per-commit durable sync and page-publication costs.
- The current `.ckpt` latest/visible pair still lacks a generation or checksum,
  so broader checkpoint batching or lazy unsynced writes remain unsafe until a
  torn-write-detectable durable record is designed.
- Broader ownerless gaps remain: SQL-level table-lock fault injection,
  additional native redo/checkpoint reconciliation, DDL/file lifecycle
  recovery, active-reader pressure policy, broader DDL/dictionary/allocation
  classes, and external MariaDB/RQG stress.
