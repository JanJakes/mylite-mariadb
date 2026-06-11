# Ownerless Page-Log Zero-Range Payload

## Problem

Production attribution on the ownerless concurrency branch shows the remaining
simple autocommit `INSERT` gap is dominated by ownerless page-version
publication during mini-transaction commit, not PHP startup, process startup,
or SQL row encoding. The hot simple-insert path still needs the two
history-proof page images in `mylite-concurrency.wal`; skipping those images
without a new native redo materialization proof would weaken peer-open and
no-live `.shm` rebuild recovery.

A first tail-only encoding attempt proved the WAL record format can safely
store less than a full page image, but the measured InnoDB hot pages still had
nonzero bytes near the page trailer. The stats-enabled sample stayed at
`4947967` page-log payload bytes versus the previous `4947968`, so tail-only
trimming did not materially change production write volume.

This slice reduces the byte volume of the existing page-version records by
encoding zero ranges inside a page image. It does not change which records are
published, when visible LSNs advance, or which records remain available to
readers and recovery.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_log.cc` stores page-version record
  metadata in a fixed 64-byte header with `page_size`, `flags`,
  `payload_size`, and checksum fields. The flags field was reserved but not
  previously used.
- `append_record_at_locked()` writes the page payload before the record header,
  so an incomplete tail remains invisible to readers that require a complete
  header.
- `find_latest_in_snapshot_range()`, `read_record_at_locked()`,
  `replay_in_snapshot()`, `checkpoint_locked()`, and
  `checkpoint_preserving_oldest_snapshot_locked()` are the page-log paths that
  must continue to validate record shape, checksum, physical offsets, and
  retained-record callbacks.
- `mtr0mtr.cc` and `trx0trx.cc` currently treat the rollback-segment SYS page
  and undo-header page as the exact history proof for skipping the native
  history flush. They must remain readable from the ownerless page-version WAL
  unless a future slice adds a stronger native redo recovery proof.

## Design

Use the existing page-log `flags` field to mark two compact full-page encodings:

- `k_record_flag_sparse_zero_payload`: the stored payload begins with a
  little-endian nonzero run count, followed by `(offset, length, bytes...)`
  entries for each nonzero run in the full page.
- `k_record_flag_trailing_zero_payload`: the stored payload is the nonzero
  prefix of a full page whose omitted suffix is all zero bytes.

Appends build the sparse run list in one pass and abandon sparse encoding as
soon as the encoded payload would be at least a full page. When sparse is
valid and smaller than the tail-prefix form, the record stores the sparse
payload. Otherwise, if the page has an all-zero suffix, the record stores only
the prefix. Fully dense pages remain old-style full-payload records.

Checksums are still computed over the full page image. Reads reconstruct the
full page by zero-filling the output buffer, applying the stored sparse runs or
prefix bytes, and verifying the checksum over `page_size`. A zero page is a
valid record and encodes as a zero-byte tail-prefix payload; internal absence
checks therefore use `commit_lsn == 0`, not `payload_size == 0`.

Checkpoint compaction validates retained records by reconstructing the full
page, then copies the encoded header and stored payload unchanged. Old
unflagged records remain valid when `payload_size == page_size`. Unknown
record flags, conflicting flags, malformed sparse run lists, or malformed
flagged payload sizes fail validation.

The record header size and outer log header format version stay unchanged.
This is still a branch-internal ownerless WAL format evolution: code before
this slice cannot read new flagged records, but the current code reads older
unflagged records.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, metadata, or storage-engine behavior changes.
The page-version WAL is an internal MyLite ownerless recovery file under the
database directory. The observable behavior remains that committed ownerless
page images are readable by peers, retained for active readers, and available
for no-live replay.

## Directory And Lifecycle Impact

No new files. `concurrency/mylite-concurrency.wal` can become smaller for pages
with zero ranges. The WAL still stores headers before payloads in the same file
and remains protected by the existing append, snapshot, and checkpoint
byte-range locks.

## Native Storage Impact

Native InnoDB data files and redo files are unchanged. This slice does not
claim broader native redo/checkpoint reconciliation and does not skip the
current history-proof page-version records.

## Build And Performance Impact

Append now scans each page once to produce a sparse payload when that would
beat the tail-prefix and full-page encodings. Reads pay a zero-fill and sparse
decode cost only for flagged records.

The final stats-enabled production attribution sample for 100 simple ownerless
autocommit inserts reported `796761` page-log payload bytes total, or
`7967.610` payload bytes per insert, down from the previous full-page sample's
`49479.680` payload bytes per insert. Total page-log record bytes fell from
`49672.960` to `8160.890` per insert. Page-log append time was `8.333 ms`
total (`0.083 ms/insert`) versus the previous `8.492 ms` total
(`0.085 ms/insert`), so the byte-volume reduction did not add measurable
append overhead in the focused sample.

A longer 500-insert stats-enabled sample showed the expected growth effect as
later table pages became less sparse: page-log payload bytes were
`17375.654` per insert and page-log append was `0.120 ms/insert`, with
ownerless autocommit at `1034.69 ops/s` versus ordinary autocommit at
`2072.58 ops/s`. Stats-off write-throughput samples varied more than the
append attribution counters; follow-up performance work should instrument the
remaining unaccounted prepared-step time before making broader end-to-end write
throughput claims.

## Tests And Verification Plan

- Build `mylite_ownerless_primitives_test` with `php-embedded-prod`.
- Run `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`.
- Run a reduced guarded production embedded performance attribution probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and compare page-log payload
  bytes with the previous full-page sample.
- Run the stats-off production embedded performance probe to verify throughput
  does not regress from the representation change.
- Run focused ownerless SQL selectors that exercise retained page-version WAL
  and history-proof publication.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Sparse zero-range records are read back as full page images.
- Tail-prefix records are read back as full page images.
- All-zero pages are valid records and do not look like missing pages.
- Append-session offsets advance by encoded payload size.
- Checkpoint compaction preserves encoded retained records and keeps them
  readable after truncation.
- Old unflagged full-payload records remain readable.
- No page-version publish, visibility, recovery, or checkpoint proof is
  weakened.

## Risks And Unresolved Questions

- Compact payloads do not reduce record count or record-header bytes.
- Sparse decode adds work to WAL reads of compact records; hot write samples
  currently benefit because payload write volume drops much more than decode
  work grows.
- Broader native redo/checkpoint reconciliation remains required before MyLite
  can safely remove current history-proof page-version records.
