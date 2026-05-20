# Compact Buffered Page Undo

## Problem

Cross-statement buffered update rewrite preserves savepoint rollback by copying
the preimage of each older buffered page before mutation. That originally copied
the full 4096-byte page into the per-statement undo list. The current update
benchmark rewrites one row page and one changed index-entry page per nested
statement, so undo capture remains visible after removing duplicate-key probes
and whole-page rewrite rebuilds.

Row and index-entry pages written by MyLite use a meaningful prefix plus a
zero tail. The stored checksum is computed as if all bytes after the meaningful
prefix are zero, and fresh encoders zero the page before writing that prefix.
Copying the whole tail into transient undo is therefore unnecessary for
validated buffered row and index-entry pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::capture_buffered_page_undo()` copies
  a buffered page preimage before `rewrite_active_update_pages()` mutates it.
- `restore_buffered_page_undos()` restores those preimages before rollback
  trims or flushes retained buffered page ranges.
- Row pages store their meaningful bytes through
  `MYLITE_STORAGE_FORMAT_ROW_PAYLOAD_OFFSET + row_size`.
- Index-entry pages store their meaningful bytes through
  `MYLITE_STORAGE_FORMAT_INDEX_KEY_OFFSET + key_size`.
- The active rewrite path validates row and index-entry pages before capture
  and mutation. Other page types are not part of the current rewrite fast path.

## Design

- Add a `used_size` field to each buffered-page undo entry.
- When capturing a row page, copy only the prefix through the row payload.
- When capturing an index-entry page, copy only the prefix through the key
  bytes.
- Keep full-page capture for any other page type or malformed page shape.
- On rollback restore, rebuild compact row/index preimages into a temporary
  zero-filled page and copy the saved prefix before replacing the buffered page.
- Preserve exact behavior for full-page undo entries.

## Affected Subsystems

- Active append-page buffer rollback.
- Nested statement and savepoint rollback over buffered update rewrites.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL, handler, public API, or MySQL/MariaDB compatibility behavior changes.
The restored row and index-entry pages carry the same meaningful bytes and the
same zero-tail checksum semantics as fresh MyLite page encoders.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, or lifecycle change. Compact undo lives
only in transient process memory attached to active statement checkpoints.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

The first two full CTest runs after implementation exposed transient failures
in `libmylite.embedded-storage-engine`: one bus error and one subprocess abort.
The same binary then passed from the repository root, from the CTest working
directory, through `ctest -R '^libmylite\.embedded-storage-engine$'`, through
`ctest -I 1,8`, and finally through the full storage-smoke CTest gate. No code
change was made between the isolated reruns and the final full pass.

Measured update baseline after the final pass:

- Direct primary-key updates: `12.166 us/op`.
- Prepared primary-key updates: `5.650 us/op`.

Sampled rerun:

- Direct primary-key updates: `12.336 us/op`.
- Prepared primary-key updates: `5.639 us/op`.

The sample showed `capture_buffered_page_undo()` moving out of the dominant hot
path; checksum generation remained the primary measured storage cost.

## Acceptance Criteria

- Savepoint rollback still restores buffered row and changed index-entry
  preimages after an older buffered replacement row is rewritten.
- Full-page undo remains available for non-row and non-index page types.
- Existing storage and embedded storage-engine tests remain green.
- Update profiles move full-page undo copy cost down without changing durable
  file contents.

## Risks And Open Questions

- The undo entry still reserves a fixed 4096-byte array, so this slice reduces
  hot copy volume but not transient undo-list memory footprint. A variable-size
  undo allocation would need a separate memory-management review.
- This does not reduce checksum generation, which remains the primary measured
  storage cost.
