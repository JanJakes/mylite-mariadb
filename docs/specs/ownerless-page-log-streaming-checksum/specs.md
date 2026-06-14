# Ownerless Page-Log Streaming Checksum

## Problem Statement

Ownerless page-log replay, latest-page lookup, and checkpoint scans validate
each candidate page record before trusting it. Today
`packages/libmylite/src/ownerless_page_log.cc::record_payload_status()` rebuilds
the full page image and then recomputes the checksum. That is correct, but it
turns compact sparse records into full-page allocation and copy work even when
the caller only needs to prove that the payload bytes are intact.

The remaining ownerless performance profile still shows meaningful page-log and
history-proof cost. This slice reduces scan/replay validation overhead without
changing the durable record format or weakening checksum coverage.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes modified InnoDB page
  images at mini-transaction boundaries. MyLite's ownerless page-log record
  checksum must continue to cover the full page image that came from that
  boundary.
- `mariadb/storage/innobase/trx/trx0trx.cc` uses rollback-segment and undo
  pages as current ownerless history-proof evidence before treating a commit as
  WAL-proved. This slice does not replace that proof.
- `packages/libmylite/src/ownerless_page_log.cc::append_record_at_locked()`
  stores a checksum of the full original page image, even when the on-disk
  payload is encoded as trailing-zero, sparse-zero, compact sparse, varint
  compact sparse, fill sparse, index delta, or undo delta.
- `read_non_delta_record_page_payload()` can already reconstruct all non-delta
  encodings and verify the checksum, but that path allocates and fills a full
  page-sized buffer.
- `replay_in_snapshot()`, `find_latest_in_snapshot_range()`,
  `checkpoint_locked()`, `checkpoint_if_safe_locked()`, and
  `checkpoint_preserving_oldest_snapshot_locked()` call
  `record_payload_status()` only to validate payload integrity. They do not
  need a full page image at that point.
- `read_standalone_or_rewrite_delta_payload()` rewrites delta records to
  standalone records during checkpoint. Delta validation still needs the base
  page image and is outside this bounded slice.

## Design

Add a checksum accumulator that can be fed chunks of the logical full page:
payload bytes, zero gaps, trailing zeros, and fill-sparse repeated bytes. It
computes the same checksum values accepted by existing records:

- MariaDB-backed CRC32C pair when `MYLITE_WITH_MARIADB_EMBEDDED` is enabled.
- Legacy FNV checksum for older records and non-MariaDB builds.

`record_payload_status()` will use this streaming validator for non-delta
records:

- full payload records,
- trailing-zero records,
- sparse-zero records,
- compact sparse-zero records,
- varint compact sparse-zero records,
- fill sparse-zero records.

The streaming path validates the same payload shape rules as
`read_non_delta_record_page_payload()`: non-empty run counts, monotonic
non-overlapping runs, in-page bounds, valid fill run kinds, cursor exhaustion,
and exact checksum match. Checksum mismatch, payload read failure, or malformed
payload layout remains a payload mismatch so a torn final record can still be
ignored while corrupt interior records fail closed.

Delta records keep the existing full reconstruction path because they depend on
a durable base record and checkpoint rewrite currently needs a full page image.

## Compatibility Impact

This is an internal ownerless WAL validation optimization. It does not change
SQL behavior, public `libmylite` API, page-log record format, directory layout,
or MariaDB/InnoDB compatibility claims.

## Database Directory And Native Storage Impact

No new durable files are introduced. Page-log records remain stored inside the
MyLite-owned database directory with the same headers, flags, payload bytes,
and full-page checksums.

Native InnoDB page images remain the checksum authority. This slice changes
only how MyLite verifies existing encoded payloads during page-log scans.

## Embedded Lifecycle Impact

No startup, close, recovery ownership, or process-registration behavior changes.
The optimization applies whenever ownerless page-log validation runs in an
embedded process.

## Public API, Wire Protocol, Binary Size, And Dependencies

No public API or wire-protocol changes. No new dependency. Binary-size impact is
limited to small first-party checksum/parsing helpers in `ownerless_page_log.cc`
and matching test/probe counters.

## Tests And Verification Plan

- Add page-log scan performance counters for streaming checksum records and
  logical bytes validated.
- Add primitive coverage proving a valid fill-sparse record validates through
  the streaming path.
- Add primitive coverage proving corrupt sparse records still fail closed when
  interior and remain tail-tolerant when they are the final record.
- Preserve existing legacy-checksum, corrupt-tail, corrupt-interior, checkpoint,
  replay, delta, and sparse encoding tests.
- Run focused production builds/tests for ownerless primitives and selected
  ownerless SQL recovery paths.
- Run production-build guard, format, and diff checks before commit.

## Acceptance Criteria

- Valid non-delta records validate without full-page reconstruction in
  `record_payload_status()`.
- The full-page checksum contract is unchanged, including legacy checksum
  acceptance.
- Delta records continue to use the existing reconstruction path.
- Corrupt interior sparse records return an ownerless page-log error.
- Corrupt final sparse records are ignored like existing torn tail records.
- Docs and compatibility notes identify this as a performance slice, not a
  history-proof replacement.

## Risks And Unresolved Questions

- This reduces validation work but does not remove the remaining history-proof
  page publication volume. A later slice still needs a smaller native
  history-proof mechanism.
- Streaming sparse parsing duplicates some shape checks from the reconstruction
  path. Focused tests cover the important compatibility points, and keeping the
  record format unchanged limits recovery risk.
- Delta-record validation can likely be optimized later, but that requires
  preserving base-record proof semantics and checkpoint rewrite behavior.
