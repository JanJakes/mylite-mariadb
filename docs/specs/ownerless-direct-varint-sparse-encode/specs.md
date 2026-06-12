# Ownerless Direct Varint Sparse Encode

## Problem

The ownerless varint compact sparse page-log slice reduced page-version WAL
payload bytes by storing sparse run metadata as varuint16 gaps and run sizes.
That first format slice intentionally derived the varint payload from the
already-built 16-bit compact payload. The reduced ownerless bulk-insert
production probe now shows every compact-sparse record in the hot sample
selecting the varint form, so the encoder still pays to materialize compact
payload bytes that are immediately discarded.

This slice removes that extra hot-path materialization without changing the
page-version WAL record format.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::
  mtr_t::ownerless_page_writes_publish()` publishes ownerless page images at
  mini-transaction boundaries after native redo has been written.
- `packages/libmylite/src/database.cc::append_ownerless_page_version()` routes
  those page images into the ownerless page-log append session for
  `concurrency/mylite-concurrency.wal`.
- `packages/libmylite/src/ownerless_page_log.cc::
  encoded_payload_size_for_page()` selects full, trailing-zero, legacy sparse,
  compact sparse, or varint compact sparse payloads before append.
- `packages/libmylite/src/ownerless_page_log.cc::
  build_compact_sparse_zero_payload()` previously built the 16-bit compact
  payload first, and `build_varint_compact_sparse_zero_payload()` then parsed
  those bytes to build the smaller varint candidate.
- The latest focused bulk-insert stats-enabled probe before this slice reported
  all `152` bulk ownerless page-log records as varint compact sparse records,
  with the same page-version count and payload shape repeated across runs.

## Design

Keep the existing page-version WAL flags and payload formats unchanged:

```text
uint16_t run_count
repeated run_count times:
  varuint16 zero_gap_from_previous_run_end
  varuint16 run_size
  uint8_t run_bytes[run_size]
```

Change only the encoder construction order for compact-sparse candidates:

1. Scan the page once and validate that every nonzero run can be represented by
   the 16-bit compact format.
2. During that scan, compute the would-be 16-bit compact payload size and
   materialize the varint payload directly from the source page bytes.
3. If the varint payload is smaller than the computed compact size, return the
   varint payload and mark the selected compact-sparse candidate as varint.
4. If the varint payload is not smaller, scan the page a second time to
   materialize the 16-bit compact payload.
5. Preserve the existing legacy sparse and trailing-zero fallback ordering.

The hot path avoids both the compact byte materialization and the compact
payload parse. The compact-fallback path can pay a second page scan, which is
acceptable because that path is not the measured ownerless insert hot shape and
still preserves the old compact bytes exactly.

## Scope And Non-Goals

In scope:

- first-party ownerless page-log encoder implementation,
- unchanged compact and varint sparse record bytes,
- primitive readback/stat coverage for existing sparse record families, and
- production performance evidence for page-log append encode time.

Out of scope:

- changing WAL flags, record headers, checksums, or decoder behavior,
- skipping page-version records,
- changing page publication, visibility, checkpoint, or recovery ordering,
- solving the history-proof SYS/UNDO payload volume.

## Compatibility Impact

No SQL behavior, public C API behavior, PHP/mysqli behavior, wire-protocol
behavior, or MariaDB native storage format changes. Existing full,
trailing-zero, legacy sparse, 16-bit compact sparse, and varint compact sparse
WAL records remain readable because only the in-memory construction of newly
appended varint payloads changes.

## Directory And Lifecycle Impact

No new files are added. The durable ownerless page-version WAL remains
`concurrency/mylite-concurrency.wal`, and closed-directory copy/reopen behavior
is unchanged.

## Native Storage Impact

Native InnoDB redo, undo, checkpoints, buffer-pool state, and page publication
boundaries are unchanged. The page-log decoder still reconstructs the full page
and verifies the full-page checksum before returning or replaying a record.

## Build And Performance Impact

No dependency, build-profile, or binary-layout change. The expected win is
limited to encode CPU for pages that select varint compact sparse payloads.
Page-log payload bytes, page-log record-header bytes, record counts, and
payload-write volume should remain comparable to the previous varint format.

The reduced production stats-enabled probe after this slice reported:

- autocommit append records: `302`, all varint compact sparse;
- autocommit page-log payload: `609070` bytes, matching the previous sample;
- autocommit append encode time: `3.574 ms`, versus `4.312 ms` in the previous
  post-empty-page-write-leave sample;
- bulk append records: `152`, all varint compact sparse;
- bulk page-log payload: `289499` bytes, matching the previous sample;
- bulk append encode time: `2.293 ms`, versus `2.757 ms` in the previous
  post-empty-page-write-leave sample.

Total append time and row throughput remain noisy because payload-write timing
varies substantially between short local samples. This slice is therefore an
encode-cost reduction, not evidence that write throughput is solved.

## Test And Verification Plan

- Build the production PHP-embedded ownerless primitive, SQL, and performance
  probe targets.
- Run ownerless primitive page-log coverage to prove varint, compact fallback,
  legacy sparse fallback, trailing-zero, checkpoint, and readback behavior.
- Run focused ownerless SQL selectors for history-page WAL proof,
  native-support page WAL elision, and multi-row insert visible fast path.
- Run a reduced production stats-enabled performance probe and compare
  page-log append encode time, payload bytes, and varint record counts.
- Run production-build guards, formatting, and whitespace checks before commit.

## Acceptance Criteria

- Existing page-log primitive tests pass unchanged.
- Varint compact sparse records still count as compact sparse and sparse-zero
  records in append stats.
- The same hot samples report unchanged record counts and payload bytes.
- Append encode time is lower or comparable for varint-selected ownerless
  insert samples.
- No durable file format, SQL behavior, or recovery-order change is introduced.

## Risks And Unresolved Questions

- Very small samples are noisy; payload-write timing can hide encode wins in
  total append time.
- 16-bit compact fallback pages now materialize compact bytes after the initial
  validation scan. That is acceptable for the current measured workload but
  should be revisited if broad workloads show many compact-non-varint pages.
- The larger ownerless write-throughput target remains history-proof
  SYS/UNDO representation and broader native redo/checkpoint reconciliation.
