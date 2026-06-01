# Dirty Leaf Checksum Bound Cache

## Problem

Prepared-insert profiling still reports `87,176` dirty `index-leaf` checksum
refreshes from `refresh_dirty_buffered_page_checksum`. Those refreshes are
required publication work, but the generic refresh helper still reclassifies
the page and reparses index-leaf key size, entry count, and used-byte bounds
from the dirty-buffer page bytes immediately before hashing.

The previous dirty-entry leaf-fill cache slice already validates and caches
resident index-leaf entry count and capacity after dirty-buffer admission or
replacement. The writer can reuse that same validated resident-page state to
avoid redundant metadata parsing before computing the required checksum.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is involved.
- `write_dirty_page_buffer_entry()` publishes checksum-dirty dirty-buffer
  entries through `refresh_dirty_buffered_page_checksum()` before writing the
  page to the primary file.
- `refresh_dirty_buffered_page_checksum()` is intentionally generic because it
  also handles append-buffer rows, index-entry pages, roots, branches, and
  fallback callers.
- Dirty-buffer entries now refresh page type and index-leaf fill facts after
  every admission and replacement path that changes page bytes.

## Design

Extend the dirty-buffer entry's cached index-leaf facts with key size and
used-byte bounds. Add an entry-aware dirty checksum refresh helper used only by
dirty-buffer publication:

- when the entry is a cached valid index leaf, refresh the checksum using the
  cached key size, entry count, capacity, and used bytes;
- record the same test-hook checksum family/source counters as the generic
  helper;
- keep the generic `refresh_dirty_buffered_page_checksum()` path for entries
  without cached valid leaf facts and for all non-leaf page families; and
- keep append-buffer, copy-for-read, maintained-root, journal, and durable-read
  validation paths unchanged.

The checksum bytes and publication timing do not change. This slice removes
redundant writer-side metadata parsing only when the dirty-buffer entry already
owns equivalent validated state for the same page image.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata behavior, transaction semantics, recovery
semantics, or error-surface changes.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or embedded lifecycle change.
Dirty pages still publish checksum-valid bytes before primary-file writes.

## Safety Boundary

This slice does not remove checksum refreshes, full-page checksum validation,
maintained-root planning validation, recovery-journal validation, or durable
reader validation. The entry-aware path applies only to dirty-buffer writer
publication and falls back to the existing generic parser whenever cached valid
index-leaf bounds are unavailable.

## Test And Verification Plan

- Extend focused dirty leaf buffering coverage to assert cached key size and
  used-byte facts are populated after admission.
- Extend dirty-page publication checksum-source coverage to publish a
  checksum-dirty cached index leaf and verify the same checksum-source family
  counters and readable flushed page.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Dirty-buffer entry cached leaf facts include key size and used-byte bounds.
- Dirty-buffer publication uses cached leaf bounds for valid cached index leaves
  while preserving checksum-source/family accounting.
- Prepared-insert structural counters remain unchanged, including required
  checksum refresh counts and protected maintained-root decodes.

## Risks

- A stale cached bound would publish the wrong checksum span. The implementation
  keeps cache refresh centralized in the existing page-type refresh path and
  falls back to generic parsing unless the cached leaf facts are marked valid.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `320.62 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `339.06 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,994,522` bytes with `478` archive members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  completed. The sampled prepared insert step was `89.455 us/op` while the
  host load average was about `18-19`, so wall-clock timing was treated as
  noisy. Structural counters stayed unchanged at `8` full-page checksum calls,
  `127,063` zero-tail checksum calls, `5` protected maintained-root decodes,
  `21,031` dirty leaf pressure admissions, `66,144` dirty leaf merge direct
  writes, `87,176` index-leaf dirty refreshes, `31,938` pressure-context
  builds, and `19,053` planned stores. The new cached-bound counter reported
  `87,176` cached index-leaf refreshes.
