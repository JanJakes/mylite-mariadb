# Page Checksum Span Fast Path

## Problem

The current update performance profile spends measurable CPU in page checksum
generation after buffered update rewrite removed most physical append writes
from the hot loop. `checksum_page_zero_tail()` hashes each used byte with a
branch that checks whether the byte falls inside the checksum field. The same
per-byte branch exists in `checksum_page()` for full-page validation.

That branch is unnecessary for all current page formats because the checksum
field is one contiguous eight-byte span at a known offset.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The hot SQL path enters MariaDB through
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` and then calls
  `mylite_storage_update_row_with_index_entry_changes()`.
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()` now
  rewrites still-buffered replacement row and changed index pages in memory.
- `packages/mylite-storage/src/storage.c::encode_row_page()` and
  `encode_index_entry_page()` call `checksum_page_zero_tail()` for those
  rewritten pages.
- A sampled one-million prepared/direct update run after the row-state cache
  slice still showed `checksum_page_zero_tail()` under row and index page
  encoding as a material remaining storage hot spot.
- All checksum offsets are fixed page-format constants in
  `packages/mylite-storage/src/storage_format.h`, and every current checksum
  slot is within the 4096-byte page.

## Design

- Preserve the FNV-1a checksum algorithm, stored checksum values, page format,
  and validation semantics.
- Hash complete pages as three spans: bytes before the checksum field, eight
  virtual zero bytes for the checksum field, and bytes after the checksum
  field.
- Hash freshly encoded pages with known-zero tails as the meaningful prefix
  split around the checksum field, then advance the checksum over the skipped
  zero tail.
- Keep the checksum code tolerant of unusual offsets so behavior stays
  equivalent if a future internal caller passes a partial-page-end checksum
  slot.
- Do not weaken decode paths: durable full-page reads still verify all 4096
  bytes, including unused bytes after the meaningful prefix.

## Affected Subsystems

- MyLite storage page encoding and full-page validation.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL, handler, public API, or MySQL/MariaDB compatibility behavior changes.
This is an internal storage checksum implementation change.

## Single-File And Lifecycle Impact

No durable file-format, checksum algorithm, companion-file, or file-lifecycle
change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Run the storage unit test and full storage-smoke CTest gate.
- Run the update performance baseline with `--phase=updates 1000 1000000`.
- Check whether sampled update profiles move checksum work down relative to
  the next storage bottleneck.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

Verification results:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 1000000`
  reported direct primary-key updates at `13.181 us/op` and prepared
  primary-key updates at `6.540 us/op`.
- A sampled one-million-update run after the final local-loop implementation
  reported direct primary-key updates at `13.232 us/op` and prepared
  primary-key updates at `6.666 us/op`. `checksum_page_zero_tail()` remained
  visible at 304 samples, so the next performance target is avoiding buffered
  page copies and the remaining meaningful-byte checksum work rather than the
  removed checksum-slot branch.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Existing corruption tests still reject modified header, row, row-state,
  index-entry, autoincrement, blob, and journal pages.
- Existing storage and embedded storage-engine tests remain green.
- Page checksum values remain format-compatible with files written before this
  slice.
- Repeated update benchmarks show checksum CPU no longer carrying the
  per-byte checksum-slot branch cost.

## Risks And Open Questions

- This does not avoid hashing the meaningful row or index payload bytes. If
  checksum work remains dominant, the next durable step is a pager design or a
  narrower rewrite-specific checksum strategy with equivalent corruption
  detection.
