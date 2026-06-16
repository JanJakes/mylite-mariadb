# Ownerless Checkpoint Generation Cache

## Problem

Ownerless autocommit writes update `concurrency/mylite-concurrency.ckpt`
several times per insert. The redo-state-backed path already avoids rereading
the legacy latest/visible payload, but each update still rereads the
checksummed generation records to discover the next record generation and
same-pair no-op state.

That record read is redundant while the current process is the only ownerless
runtime that could have written `.ckpt`. It is unsafe once another owner has
joined, even briefly, because a peer may have advanced the generation while
this process did not observe checkpoint updates.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::Command::execute()` drives
  ownerless page publication after mini-transaction commit and page LSN
  assignment.
- `mariadb/storage/innobase/trx/trx0trx.cc` drives transaction commit and the
  MyLite redo/latest and page-visible hooks used by ownerless commit
  publication.
- `packages/libmylite/src/database.cc::ownerless_persist_redo_checkpoint()`
  persists raw-latest and page-visible LSN progress from the InnoDB hooks.
- `packages/libmylite/src/database.cc::
  update_concurrency_checkpoint_lsn_from_redo_state()` holds the checkpoint
  byte-range lock, snapshots the shared redo state, and writes the monotonic
  latest/visible pair.
- `packages/libmylite/src/database.cc::
  write_concurrency_checkpoint_lsn_locked()` still reads the checksummed LSN
  record slots to choose the next generation and no-op behavior.
- `packages/libmylite/src/ownerless_process_registry.cc` advances the registry
  generation on owner allocation and release; an unchanged generation plus
  active count of one proves no peer owner joined since this owner registered.

## Design

Add a process-local cache for the current checkpoint LSN generation record.
The cache is keyed by the open checkpoint fd and process-registry generation.
It is used only by redo-state-backed checkpoint writes and only after the
writer holds the checkpoint byte-range lock.

The cache is allowed when all of these are true:

- the process registry is mapped,
- the registry active count is exactly one,
- the registry generation still equals this ownerless hook's owner generation,
  proving no peer owner joined and left since this process registered, and
- a cached record exists for the same checkpoint fd and registry generation.

If the proof fails, the helper resets the cache and uses the existing file-read
record discovery path. Direct checkpoint writers used for startup baseline
seeding, no-live native checkpoint promotion, and test-only repeated updates
also reset the cache and continue to merge against the file payload.

The cache does not skip checkpoint writes, durable syncs, or page-version WAL
syncs. It only replaces repeated record-slot reads used to discover the current
generation. Successful writes and same-pair no-op elisions update or retain
the cached generation record. Failed writes drop the cache.

## Scope And Non-Goals

In scope:

- redo-state-backed ownerless checkpoint hot-path writes,
- a process-local generation cache under a single-owner proof,
- performance counters for cache hits, and
- focused SQL coverage with forced `.shm` rebuild recovery.

Out of scope:

- cross-process group commit,
- lazy or batched durable checkpoint publication,
- changing the `.ckpt` record format,
- eliding native page-version proof pages, and
- improving ordinary non-ownerless PHP/PHPUnit statement execution.

## Compatibility Impact

No SQL behavior, C API behavior, PHP/mysqli behavior, wire-protocol behavior,
or public directory-layout behavior changes. The same latest/visible LSN pair
is written to the same `.ckpt` records with the same durable sync rule.

## Directory And Lifecycle Impact

No files are added. The cache is process-local and is reset when ownerless
native hook contexts are cleared. The durable state remains
`concurrency/mylite-concurrency.ckpt` and `concurrency/mylite-concurrency.shm`.

## Native Storage Impact

Native InnoDB pages, redo, checkpoint, and recovery behavior are unchanged.
The optimization is in MyLite-owned ownerless coordination metadata.

## Build, Size, License, And Dependencies

No dependency, license, or build-profile changes. Binary-size impact is limited
to a small transient cache and one additional internal performance counter.

## Test And Verification Plan

- Build the production PHP embedded ownerless SQL harness and performance
  probe.
- Run the focused `checkpoint-generation-cache` SQL case and nearby checkpoint
  LSN record/no-op/legacy-elision cases.
- Run ownerless single-owner commit-path selectors.
- Run hook-build visible and redo checkpoint crash cases.
- Run a reduced production performance probe with ownerless page-publish stats
  enabled and verify generation-cache hit counters appear.
- Run production build guards, format, and whitespace checks.

## Acceptance Criteria

- Single-owner ownerless inserts record checkpoint generation-cache hits.
- Forced `.shm` rebuild after cached checkpoint publication recovers committed
  rows.
- Direct checkpoint update paths still reread the file and reset the cache.
- Peer-owner uncertainty disables the cache by active-count or generation
  mismatch.
- Performance output includes generation-cache hit counters.

## Risks And Follow-Up

- The cache removes a small repeated record-read cost; larger remaining costs
  are native mini-transaction publication, page-version WAL append/sync, and
  durable checkpoint sync.
- Broader cross-process group commit and checkpoint batching remain planned.
- External MariaDB/RQG stress and broader native redo/checkpoint
  reconciliation remain separate ownerless completion work.
