# Ownerless Index Fill-Sparse Page Log

## Goal

Reduce ownerless write-path page-log payload for user B-tree index pages by
allowing the existing fill-sparse zero-payload codec to encode native
`FIL_PAGE_INDEX` page images when it is smaller than the current compact sparse
format.

## Non-Goals

- Do not change ownerless page-version visibility, read pins, checkpoint
  eligibility, native redo handling, or InnoDB mini-transaction boundaries.
- Do not treat application index pages as native support pages. Index page
  records still require a snapshot boundary before oldest-reader-preserving
  checkpoint can discard earlier state.
- Do not extend the codec to RTREE, instant index pages, BLOB, undo, or
  allocation/system page classes in this slice.
- Do not claim ownerless concurrency complete.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/fil0fil.h` defines `FIL_PAGE_TYPE` at
  offset 24, `FIL_PAGE_INDEX = 17855`, `FIL_PAGE_UNDO_LOG = 2`, and
  `FIL_PAGE_TYPE_SYS = 6`. The comment notes that uncompressed B-tree index
  pages are guaranteed to carry `FIL_PAGE_INDEX`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit_log()` writes
  `FIL_PAGE_LSN` into modified buffer pages before publishing ownerless page
  writes. The page image remains the authoritative physical payload boundary.
- `mariadb/storage/innobase/buf/buf0flu.cc:buf_flush_init_for_writing()` treats
  `FIL_PAGE_INDEX` as a normal page-flush class whose checksum is computed
  from the complete page image.
- `packages/libmylite/src/ownerless_page_log.cc` owns ownerless page-log
  record flags, sparse/fill-sparse encoding, decoding, page-type peeking, and
  checkpoint snapshot-boundary classification.

The preceding SYS-only fill-sparse slice reduced history-proof SYS payload to
about `72` bytes per insert. Production attribution still shows user/index
page payload as the next large measured bucket, so the next bounded work is to
apply the same byte-exact codec to B-tree index page images without changing
their retention class.

## Compatibility Impact

There is no SQL, C API, mysqli, WordPress, or wire-protocol behavior change.
The ownerless page-version WAL is internal MyLite directory state, and existing
records remain readable. New index fill-sparse records require a MyLite reader
that already understands `k_record_flag_fill_sparse_zero_payload`.

## Design

In `packages/libmylite/src/ownerless_page_log.cc`, extend the fill-sparse
candidate predicate from `FIL_PAGE_TYPE_SYS` to
`FIL_PAGE_TYPE_SYS || FIL_PAGE_INDEX`. The encoder still selects fill-sparse
only when the encoded payload is smaller than the existing compact or varint
compact sparse payload. Full-page checksums continue to validate the
reconstructed page image during reads and replay.

Checkpoint page-type peeking already understands fill-sparse records. Because
`FIL_PAGE_INDEX` is intentionally absent from
`record_page_type_is_native_support_state()`, oldest-snapshot checkpointing
must continue to retain index records unless an older index-page boundary is
available.

## File Lifecycle

No new file is introduced. Durable state remains in
`concurrency/mylite-concurrency.wal` under the MyLite database directory.

## Embedded Lifecycle And API

There is no public API or open/close behavior change. Ordinary exclusive opens
stay on the native path unless retained ownerless WAL requires replay.

## Build, Size, And Dependencies

No dependency or embedded profile change is introduced. The implementation is a
small first-party predicate/test/docs change in the ownerless page-log module.

## Test Plan

- Add primitive coverage for a `FIL_PAGE_INDEX`-typed page with repeated
  nonzero fill bytes:
  - fill-sparse is selected and attributed to the index payload bucket,
  - latest-page and direct-record reads reconstruct the exact page,
  - oldest-snapshot checkpointing remains busy without an index boundary,
  - a boundary index record permits checkpoint while retaining the boundary and
    later index record.
- Run focused ownerless primitive and ownerless SQL history/replay selectors.
- Run stats-enabled and stats-off production performance probes to record
  index payload impact.
- Run relevant hook, stress, format, production-build, and whitespace checks.

## Acceptance Criteria

- Index fill-sparse records reconstruct exact page bytes through both read
  paths.
- `FIL_PAGE_INDEX` fill-sparse records remain snapshot-sensitive for checkpoint
  retention.
- Focused embedded ownerless SQL tests pass with index fill-sparse enabled.
- Production attribution shows reduced index payload or documents why the
  measured SQL path does not produce fill-sparse-friendly index images.
- Docs and compatibility notes identify the scope and remaining gaps.

## Implementation Evidence

The primitive page-log test now constructs a `FIL_PAGE_INDEX` image with
repeated nonzero fill regions, verifies fill-sparse selection and exact replay,
and proves oldest-snapshot checkpoint remains busy without an index-page
boundary. The boundary variant keeps the same fill-sparse index records and
allows checkpoint only when a boundary record is retained.

Focused production SQL selectors passed for the history WAL proof,
native-support page WAL elision, and multi-row insert visible fast path. The
reduced 100-row stats-enabled production probe reported `1779.010` index
payload bytes, `159.340` undo-log bytes, and `72.030` SYS bytes per ownerless
autocommit insert. Fill-sparse selected `1.000` record per insert, still the
SYS page, so the representative simple insert index image is not dominated by
repeated fill runs. The next performance slice should target native
commit/page-publication cost or a stronger user/index representation rather
than broader fill-run scanning.

## Risks And Open Questions

Earlier exploratory generic fill-sparse work produced an unsafe index-page
result before the final SYS-only gate. This slice enables index fill-sparse
only after adding primitive checkpoint-retention coverage and passing focused
SQL replay. The remaining risk is performance value, not byte-level replay
correctness: the representative insert index image did not select fill-sparse,
so later user/index payload work needs a stronger representation than repeated
fill-run encoding.
