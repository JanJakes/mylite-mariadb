# Ownerless Compact Sparse Page Log

## Problem

The ownerless page-log payload attribution slice proved that the remaining
simple-autocommit write gap is concentrated in retained page-version WAL
payload. The latest production CI attribution sample reported about `7971.590`
payload bytes per ownerless autocommit insert, all encoded as sparse-zero page
records, split mostly across the rollback-segment `FIL_PAGE_TYPE_SYS` history
proof page and the clustered index page.

The current sparse-zero record format uses 32-bit offsets and 32-bit lengths for
every non-zero run. InnoDB pages in the embedded profile are normally well below
64 KiB, so that metadata is larger than needed for the hot path.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc` owns the MyLite page-version
  WAL format. `append_record_at_locked()` writes a 64-byte record header,
  computes a checksum over the full reconstructed page image, then writes a
  full, trailing-zero, or sparse-zero payload.
- `encoded_payload_size_for_page()` currently chooses sparse-zero only when the
  32-bit run payload is smaller than the trailing-zero representation.
- `read_record_page_payload()` reconstructs a full page before checksum
  validation and before handing the image to page-version readers, checkpoint,
  or replay.
- The InnoDB publish hooks in `mariadb/storage/innobase/mtr/mtr0mtr.cc` decide
  which pages must be retained. This slice does not alter those hooks or the
  history-proof contract.

## Design

Add a compact sparse-zero record flag whose payload uses 16-bit little-endian
run metadata:

- `uint16_t run_count`,
- repeated `uint16_t run_offset`, `uint16_t run_size`, followed by run bytes.

The append path will prefer compact sparse-zero payloads when every non-zero run
fits in 16-bit offset/size fields and the compact payload is smaller than the
trailing-zero representation. It will fall back to the existing 32-bit
sparse-zero format for large pages or runs that cannot be represented compactly,
and then to the existing trailing-zero/full-page choices.

The full-page checksum remains unchanged. Readers validate record shape, rebuild
the full page from either sparse format, and verify the checksum over the full
page. Existing full, trailing-zero, and 32-bit sparse records remain readable.
Old binaries are not guaranteed to read new compact-sparse records.

Stats continue to report compact records under the existing sparse-zero totals
so before/after payload bytes stay comparable. The perf probe also gains
compact-sparse subset counters and bytes so CI can show whether new records use
the compact path.

## Compatibility Impact

No SQL, C API, PHP API, mysqli behavior, storage-engine semantics, or directory
layout changes. This is an internal ownerless page-version WAL encoding
extension. Newer builds read existing retained WAL records; downgrade reads of
new compact-sparse records are out of scope.

## Directory And Lifecycle Impact

No new files are added. The existing `concurrency/mylite-concurrency.wal`
record stream can contain old and new sparse encodings. Append ordering,
snapshot reads, checkpoint locking, and reclaim decisions are unchanged.

## Native Storage Impact

Native InnoDB files, redo, checkpoints, and history proof publication are
unchanged. This slice reduces the MyLite companion WAL bytes for page images; it
does not claim broader native redo/checkpoint reconciliation.

## Build And Performance Impact

Stats-disabled writes add one extra compact sparse-encoding attempt before the
existing sparse-zero fallback. The expected hot-path benefit is lower page-log
payload bytes and less checksum/pwrite pressure for sparse InnoDB pages. The
slice may not reduce native InnoDB row-insert or commit time, and it does not
reduce the number of published history-proof pages.

Local production verification used the `php-embedded-prod` preset. A reduced
100-row stats-enabled ownerless attribution probe reported all `3.020`
page-log append records per insert on the compact sparse path, with payload
falling to `6701.670` bytes per insert from the previous sparse-zero
attribution sample's roughly `7969.470` to `7971.590` bytes per insert. The
new sample split those bytes into `4174.940` `FIL_PAGE_TYPE_SYS` bytes,
`2302.290` index bytes, `223.980` undo-log bytes, and `0.460`
space/allocation metadata bytes per insert.

Longer 1000-row production samples reported stats-off ownerless autocommit at
`1374.23` ops/s versus ordinary autocommit at `4031.68` ops/s, and
stats-enabled ownerless autocommit at `1434.30` ops/s versus ordinary
autocommit at `4008.89` ops/s. The 1000-row stats-enabled attribution sample
also showed sample-shape variance: `2.875` compact sparse records per insert
plus occasional full index records, with `14903.235` payload bytes per insert.
The defensible conclusion is that compact sparse metadata reduces zero-heavy
page-log payload and can improve sustained local ownerless autocommit
throughput, while the next performance target remains reducing retained
history-proof/native-support page volume or native commit cost.

## Tests And Verification Plan

- Extend ownerless primitive coverage so normal sparse pages use compact
  sparse-zero records, read back correctly, and checkpoint correctly.
- Add fallback coverage for a page whose non-zero run cannot be represented in
  16-bit sparse metadata, proving the existing 32-bit sparse format remains
  readable.
- Extend the embedded performance probe output with compact-sparse counters and
  run a reduced production stats-enabled ownerless attribution sample.
- Run focused ownerless primitive and embedded ownerless SQL coverage, CI
  production-build audit, format check, and `git diff --check`.

## Acceptance Criteria

- Existing retained full, trailing-zero, and 32-bit sparse records remain
  readable.
- New compact sparse-zero records reconstruct to the exact original page and
  pass full-page checksum validation.
- Checkpoint and latest-page lookup preserve behavior across compact sparse
  records.
- Production attribution shows compact sparse-zero records in the hot path and
  reports reduced ownerless page-log payload bytes per insert, or documents why
  the current page shapes do not benefit.

## Risks And Unresolved Questions

- This is a WAL encoding extension; old binaries may reject compact-sparse
  records through the existing unknown-flag validation.
- If actual non-zero page data, not run metadata, dominates the hot-path pages,
  throughput improvement may be modest.
- A later slice may still need a smaller history-proof representation or a
  native redo/checkpoint proof to remove whole proof page images.
