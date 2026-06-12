# Ownerless Page-Log Clean Sync Elision

## Problem

The ownerless visible-page hook must make page-version WAL records durable
before it publishes a page-visible LSN to shared state. The previous
performance profiling slice showed that this sync can dominate the
stats-enabled ownerless autocommit insert path on CI, but blindly dropping or
deferring that sync would weaken crash recovery.

There is still a safe smaller optimization: when a process has already synced
the same ownerless page-version WAL image, and neither the file size nor the
page-log header generation has changed, a later clean visible-publication path
does not need another `fdatasync()`/`fsync()` before publishing the same
boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` drives the ownerless commit path
  through MyLite's InnoDB page-publication hooks before commit-visible state is
  exposed to peers.
- `packages/libmylite/src/database.cc::ownerless_innodb_pages_visible_hook()`
  syncs `mylite-concurrency.wal`, publishes the page-visible LSN in shared redo
  state, then durably updates `mylite-concurrency.ckpt`.
- `packages/libmylite/src/ownerless_page_log.cc::sync_at_common()` validates
  the page-log header under the append/snapshot byte-range lock and then calls
  `sync_file_data()`.
- `packages/libmylite/src/ownerless_page_log.cc::checkpoint_locked()`,
  `checkpoint_preserving_oldest_snapshot_locked()`, and
  `checkpoint_if_safe_locked()` increment the page-log header generation before
  truncating or rewriting the WAL tail, then call `sync_file()` on the file.
  The header generation therefore distinguishes a checkpoint rewrite from a
  clean unchanged file at the same byte size.

## Design

Add an internal page-log sync primitive:

```c
int mylite_ownerless_page_log_sync_initialized_if_changed_at(
    int fd,
    uint64_t log_offset,
    uint64_t known_synced_end_offset,
    uint64_t known_synced_generation,
    uint64_t *out_current_end_offset,
    uint64_t *out_current_generation,
    int *out_synced
);
```

The primitive:

- takes the same append/snapshot byte-range lock as the existing sync path,
- validates the initialized page-log header,
- reads the current file size and header generation while holding that lock,
- skips data sync only when both values exactly match the caller's known synced
  anchor, and
- returns the current size/generation plus whether it actually synced.

The visible hook keeps a process-local sync anchor keyed by `(fd, log_offset)`.
The anchor is protected by a mutex, reset when ownerless native hook contexts
are cleared, reset after opening the concurrency page log, and reset on the
shared-memory setup failure path that clears the InnoDB hook context. A
successful sync-or-skip updates the anchor; a failed call leaves it unchanged.

## Scope And Non-Goals

In scope:

- clean-sync elision for the ownerless visible-page hook,
- primitive coverage for sync, skip, and append-forced-resync cases,
- performance-probe counters showing skipped clean syncs.

Out of scope:

- cross-process group commit,
- replacing page-version WAL durability with native InnoDB redo proof,
- changing `mylite-concurrency.wal` file format,
- relaxing visible-publish crash ordering, and
- claiming ownerless autocommit insert throughput is close to ordinary native
  throughput.

## Compatibility Impact

No SQL behavior, C API behavior, PHP/mysqli behavior, wire-protocol behavior,
or public directory layout changes. The new function is first-party internal
ownerless page-log plumbing and the visible hook keeps the same durable ordering
when the WAL image has changed.

## Directory And Lifecycle Impact

No files are added. The ownerless page-version WAL remains
`concurrency/mylite-concurrency.wal`; the checkpoint file remains
`concurrency/mylite-concurrency.ckpt`. The runtime anchor is process-local and
is not durable state. Closing or resetting the ownerless native hook context
invalidates it so a later fd reuse cannot inherit a stale sync proof.

## Native Storage Impact

Native InnoDB files, redo, and checkpoint behavior are unchanged. The existing
page-visible order still syncs the MyLite page-version WAL before publishing a
new visible LSN unless the page-log size and generation prove that the same WAL
image was already synced by this process.

## Build And Performance Impact

The hot visible hook adds a mutex-protected size/generation check around an
existing syscall-heavy path. On workloads where the WAL grows for every commit,
such as the current ownerless autocommit insert benchmark, this usually still
syncs once per commit and should be treated as a foundation for later batching
work rather than the main performance fix.

Stats-enabled probes now report skipped clean sync counts so CI logs can show
whether a workload actually benefits from this elision.

## Test And Verification Plan

- Extend `ownerless_primitives_test` so initialized page-log sync coverage
  proves:
  - an uninitialized page log still errors,
  - the first size/generation call syncs,
  - a second unchanged size/generation call skips the data sync, and
  - appending a page-version record forces a sync again.
- Extend `embedded_performance_probe` output with raw and per-insert skipped
  clean sync counters.
- Run focused ownerless primitive coverage, visible-publish and visible-
  checkpoint crash coverage, a reduced production performance probe with page-
  publish stats enabled, production build guards, format check, and diff
  whitespace checks.

## Acceptance Criteria

- Existing page-log sync APIs keep their unconditional sync behavior.
- The new primitive never skips after WAL growth, WAL shrink, or page-log
  generation change.
- The visible-page hook publishes only after the sync primitive returns success.
- Crash-hook coverage for visible publish and visible checkpoint still passes.
- Performance output includes skipped clean sync counts.

## Risks And Unresolved Questions

- This slice does not remove the dominant per-autocommit sync cost when every
  commit appends new WAL records.
- A larger speedup likely requires cross-process group commit, a durable native
  redo/checkpoint proof that can replace some MyLite WAL syncs, or a broader
  redesign of page-visible publication.
- Broader ownerless gaps remain: SQL-level table-lock fault injection,
  additional native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  active-reader pressure policy, broader DDL/dictionary/allocation classes, and
  external MariaDB/RQG stress.
