# Ownerless Page-Publish Buffer Reuse

## Problem Statement

The remaining ownerless performance profile points back to native
mini-transaction page publication and native commit/checkpoint work after
earlier slices separated PHP startup, SQL dispatch, timer-driven buffer-pool
scan publication, page-log batching, sparse encoding, and page-log scan
validation. The current InnoDB publish path still allocates and frees an
aligned page-sized scratch buffer for every page image it publishes.

That allocation churn is not part of the correctness proof. Ownerless
publication must continue to pass a stable checksum-initialized full page image
to the MyLite page-version hook, but the temporary buffer used to prepare that
image can be reused by the publishing thread.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_publish()`
  publishes ownerless InnoDB page versions at mini-transaction boundaries. For
  each non-elided page it reads the source page LSN, looks up the tablespace,
  allocates an aligned buffer with `aligned_malloc(page_size, page_size)`,
  copies the page, initializes the InnoDB write checksum, calls
  `mylite_ownerless_innodb_publish_page_version()`, and frees the buffer.
- `mariadb/storage/innobase/include/mylite_ownerless_innodb_lock_hooks.h`
  declares `mylite_ownerless_innodb_publish_page_version()` as a synchronous
  callback that receives a full page pointer and size. The callback contract
  does not require the caller's scratch allocation to outlive the call.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  forwards the publish call directly to the registered MyLite callback while
  the caller-provided page pointer is live.
- `mariadb/storage/innobase/trx/trx0trx.cc` uses the ownerless history WAL
  proof only after the transaction's expected rollback-segment and undo-header
  pages were successfully published. This slice does not change that proof or
  elide those pages.
- `docs/COMPATIBILITY.md` and
  `docs/specs/ownerless-cross-process-concurrency/specs.md` currently identify
  native commit/page-publication cost and broader redo/checkpoint
  reconciliation as the remaining performance targets after the completed
  page-log encoding and scan-validation slices.

## Design

Add a small thread-local scratch page buffer in `mtr0mtr.cc` for ownerless
page-version publication:

- The first publish on a thread allocates the page-sized aligned buffer.
- Later publishes on the same thread and same physical page size reuse it.
- If the physical page size changes, the old buffer is freed and a new
  correctly aligned buffer is allocated.
- If a same-thread publish is ever re-entered while the retained buffer is
  still checked out, the nested publish falls back to a one-shot aligned
  buffer instead of overwriting the outer page image.
- Allocation failure keeps the existing skipped-allocation failure path.
- The buffer is overwritten before each synchronous publish callback and is
  retained only as process-local transient memory.

The existing page image, LSN, checksum, page-type attribution, native-support
history-proof attribution, and publish-hook behavior remain unchanged. The
buffer is not shared across threads and does not become durable state.

Add two page-write perf counters:

- publish buffer reuse hits,
- publish buffer reuse misses.

The embedded performance probe emits those counters so CI and local production
probes can verify that repeated ownerless publishes avoid repeated scratch
allocations.

## Compatibility Impact

This is an internal ownerless performance change. It does not change SQL
behavior, public `libmylite` API, MariaDB C API compatibility, page-version WAL
record format, page checksums, transaction visibility, or unsupported-surface
policy.

## Database Directory And Native Storage Impact

No durable files, directory layout, page-log records, checkpoint records, or
native storage files change. The scratch buffer is transient process memory
only. Native InnoDB pages are still copied and checksum-initialized before the
page-version hook receives them.

## Embedded Lifecycle Impact

The scratch buffer is thread-local and freed when the publishing thread exits.
It is not tied to a database directory and it is not preserved across
processes. Repeated `mylite_open()`/`mylite_close()` lifecycles in one process
may reuse the same transient buffer, which is safe because every publish
overwrites the full page image before the synchronous hook call.

## Public API, Wire Protocol, Binary Size, And Dependencies

No public API, wire-protocol, dependency, or build-profile change. Binary-size
impact is limited to a small ownerless helper class and two perf counters in
existing embedded instrumentation.

## Tests And Verification Plan

- Build the MariaDB embedded archive after editing `mariadb/`.
- Build focused production targets for the embedded performance probe and
  ownerless SQL harness.
- Run focused ownerless SQL selectors that exercise the history WAL proof,
  native reclaim, visible checkpoint recovery, and native file-operation marker
  drain.
- Run a reduced stats-enabled production embedded performance probe and verify
  publish buffer reuse counters are emitted and show reuse hits on repeated
  publishes. A phase whose stats were reset after the scratch buffer was
  already warmed may legitimately report zero misses.
- Run the production-build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Ownerless page publication no longer allocates/frees a scratch page buffer
  for every published page on a same-page-size publishing thread.
- Existing page image, checksum, page-type attribution, history-proof, and
  failure semantics remain unchanged.
- Perf logs expose reuse hits and misses.
- Focused ownerless correctness selectors still pass under production builds.
- Docs identify the slice as a bounded performance improvement, not a
  replacement for history-proof or redo/checkpoint reconciliation work.

## Risks And Unresolved Questions

- This reduces allocation churn but does not reduce page-version count,
  history-proof record count, or the remaining native redo/checkpoint proof
  gap.
- Threads that publish only one page still pay one allocation. That is expected
  and bounded to one scratch page per publishing thread and page size.
- If a future asynchronous page-version hook is introduced, it must copy the
  page before returning or provide a new lifetime contract. The current hook is
  synchronous.
