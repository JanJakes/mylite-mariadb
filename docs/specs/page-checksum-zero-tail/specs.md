# Page Checksum Zero-Tail Fast Path

## Problem

The update benchmark writes a replacement row page, row-state page, and index
entry page for each inline update. Sampling shows the hot write path is now
dominated by FNV checksum work over the unused zero tail of each freshly encoded
16 KiB page. The durable page format still needs full-page checksums for
corruption detection, but newly encoded pages know where their meaningful bytes
end and that the rest of the page is zero.

## Source Findings

- MyLite storage format currently uses `MYLITE_STORAGE_FORMAT_CHECKSUM_FNV1A64`
  for all storage pages.
- `packages/mylite-storage/src/storage.c::checksum_page()` computes the
  reference full-page FNV-1a checksum and zeroes the checksum field logically.
- `encode_row_page()`, `encode_row_state_page()`, and
  `encode_index_entry_page()` zero the full page before writing fixed metadata
  and a bounded payload/key prefix.
- `write_inline_update_pages()` calls those encoders for the update hot path.
- Read/decode functions must keep using the full-page checksum because persisted
  pages may be corrupt anywhere, including in the unused tail.

## Design

Add a write-side checksum helper that is equivalent to the full-page checksum
when all bytes after a supplied used-prefix length are zero. The helper:

- hashes the actual prefix bytes with the same checksum-field zeroing rule;
- multiplies the running FNV state by `FNV_PRIME ^ zero_tail_length` modulo
  `2^64`, which is exactly what hashing zero bytes would do;
- falls back to the full-page helper for invalid prefix lengths.

Use the helper only in page encoders that have just zeroed the whole page and
know the last written byte. Keep all decode-time validation on
`checksum_page()`.

## Compatibility Impact

No SQL, API, storage routing, or on-disk format change. Checksums remain
byte-for-byte identical to the existing FNV-1a page checksum.

## Single-File And Lifecycle Impact

No file lifecycle change. The optimization affects transient page encoding
before bytes are written to the primary `.mylite` file.

## Test And Verification Plan

- Keep existing corruption tests, which verify that decode still checks full
  pages.
- Build storage-smoke targets.
- Run focused storage and embedded storage-engine smoke tests.
- Run the full storage-smoke CTest suite.
- Run the local performance baseline at small and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Newly encoded row, row-state, index-entry, blob, index-leaf, and
  autoincrement pages produce the same checksum as the full-page helper.
- Decode paths continue to reject corrupted bytes anywhere in the page.
- The local performance baseline improves update-heavy write timings, where
  zero-tail checksumming dominated the previous sample.

## Risks

- A wrong used-prefix length would produce a checksum that does not match the
  persisted page. Limit use to encoders with straightforward final byte offsets.
- This improves append-page CPU cost, but it does not remove the remaining
  `pwrite()` cost or replace the planned pager/WAL work.
