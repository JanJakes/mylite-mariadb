# Dirty Page Buffer Leaf Growth Replacement Fast Path

## Problem

After `9a299c46`, the prepared-insert replacement profile separates index-leaf
rewrites into `3,762` append replacements and `62,630` interior single-entry
insert replacements, with no remaining valid `other` leaf replacements in the
100,000-iteration smoke run.

`store_dirty_page_in_buffer_at_pressure_write_site()` still copies the full
4 KiB incoming page over the resident dirty-buffer slot for all index-leaf
replacements. For single-row maintained inserts, that is unnecessary when the
incoming leaf page is byte-proven to be the resident leaf page with exactly one
fixed-width cell inserted.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- Index leaf pages store fixed metadata at offsets `24` through `64`, with
  entry count at `MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_COUNT_OFFSET`, used
  bytes at `MYLITE_STORAGE_FORMAT_INDEX_LEAF_USED_BYTES_OFFSET`, checksum at
  `MYLITE_STORAGE_FORMAT_INDEX_LEAF_CHECKSUM_OFFSET`, and payload at
  `MYLITE_STORAGE_FORMAT_INDEX_LEAF_PAYLOAD_OFFSET`.
- Leaf cells are fixed width within a page:
  `MYLITE_STORAGE_FORMAT_INDEX_LEAF_ENTRY_HEADER_SIZE + key_size`.
- `dirty_page_buffer_replacement_leaf_change_class()` already proves the
  single-cell growth shape by validating key size, entry count, used bytes,
  stable metadata, stable unused tail bytes, and payload equality around one
  inserted cell.
- `insert_index_leaf_entry_into_page()` mutates an already decoded leaf page
  with `memmove()` and fixed-cell writes, then updates entry count, used bytes,
  and the checksum slot before the dirty maintained write path publishes the
  page.

## Design

Add a production dirty-buffer replacement fast path for resident index-leaf
pages when the incoming page is byte-equivalent to the resident page with one
new fixed-width leaf cell inserted at any position, including the tail append
position.

The helper must reject pages with invalid leaf metadata, changed key size,
changed stable metadata, unchanged or non-single-cell entry-count growth,
changed unused tail bytes, or payload changes that cannot be represented as one
inserted cell. When it matches, it mutates the resident dirty-buffer page in
place:

- shift the suffix cells with `memmove()` when the insert is interior;
- copy the incoming inserted cell;
- copy the incoming entry count, used bytes, and checksum fields; and
- update the resident `checksum_dirty` flag.

Existing replacement counters still run before and after replacement, so the
profile continues to report append/insert shape classes independently from the
fast-path counter. Non-leaf replacements, leaf shrink/refold rewrites,
same-shape rewrites, invalid leaf metadata, and broader structural leaf changes
keep the existing full page-copy path.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable bytes change. Eligible replacements
produce the same resident dirty-buffer page image as the full copy path.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, pressure flush,
statement commit, dirty-buffer pressure policy, and embedded lifecycle behavior
remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Test-hook builds add one counter accessor and benchmark
row. Production builds add one conservative leaf replacement helper and avoid
full page copies only for byte-proven single-cell growth replacements.

## Tests And Verification

- Add a storage test-hook case that replaces a resident leaf page with an
  interior single-cell insert and verifies:
  - the resident page bytes match the incoming page exactly after replacement;
  - the fast-path counter increments;
  - existing replacement family and leaf-change counters still report the
    replacement.
- Verify a same-shape leaf rewrite still takes the full replacement path and
  does not increment the fast-path counter.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Single-cell index-leaf growth replacements update the resident dirty-buffer
  page without a full 4 KiB copy.
- Broader leaf rewrites keep the existing full copy path.
- Dirty-buffer replacement counters, flush behavior, rollback behavior,
  checksum-dirty semantics, and branch replacement fast paths remain correct.
- The prepared-insert benchmark reports the number of applied leaf growth fast
  replacements.

## Risks

- The fast path depends on fixed leaf page offsets. The helper must compare all
  page bytes except entry count, used bytes, checksum, and the inserted payload
  cell before applying the narrow update.
- The helper must mutate the resident page into the exact incoming page image,
  including the checksum field, so later dirty-buffer reads and flushes remain
  indistinguishable from the full-copy path.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 292.22 seconds after the single-pass insert-position scan update.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  built `libmariadbd.a` at 32.40 MiB with `PLUGIN_MYLITE_SE=STATIC`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in 342.05 seconds after the single-pass insert-position scan update.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported the prepared insert step at `79.547 us/op`, `66,392` leaf growth
  fast replacements, `3,762` append leaf replacements, `62,630` insert leaf
  replacements, and `0` other leaf replacements.
