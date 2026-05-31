# Maintained Root Insert Replacement Fast Path

## Problem

Prepared-insert maintained-root decodes are now limited to protected planning,
recovery-journal validation, and one root-to-leaf read. The same profile still
reports `668` checksum-dirty `index-root` dirty-buffer replacements. Most of
those replacements are writer-side maintained-root insert pages produced after
`plan_maintained_index_root_inserts()` has decoded the root, computed the
planned insertion slot, and journal validation has protected the root page.

The current dirty-buffer replacement path copies the full `4096` byte root page
when the same maintained-root page is already resident. For planned single-page
root inserts, the replacement differs by one inserted fixed-width root cell plus
the entry-count, used-bytes, and checksum fields. The next safe slice is to
prove that shape and update the resident dirty-buffer page in place.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is modified.
- `plan_maintained_index_root_inserts()` decodes maintained roots before write
  journal setup and records the planned entry count and insert position.
- `write_maintained_index_root_inserts()` reads the planned root, mutates it
  with `insert_maintained_index_root_entry(..., refresh_checksum = 0)`, then
  writes it through `pager_write_buffered_maintained_index_page()`.
- `store_dirty_page_in_buffer()` replaces an already-resident dirty page by
  trying narrow index-leaf and index-branch fast paths, then falling back to a
  full page copy. It has no maintained-root insert fast path today.

## Design

Add a maintained-root single-insert dirty-buffer replacement proof:

- require both resident and incoming pages to be maintained index roots with
  matching root metadata and key size;
- require the incoming entry count and used bytes to be exactly one root cell
  larger than the resident page;
- compare all unchanged header bytes, flags, overflow-tail bytes, unused tail
  bytes, and unchanged payload prefix/suffix bytes;
- locate the one inserted cell by comparing fixed-width root cells;
- update the resident payload with one `memmove()` plus one inserted-cell copy;
  and
- copy only the entry-count, used-bytes, and checksum fields from the incoming
  page before marking the resident dirty state.

If any proof check fails, keep the existing full-page replacement path. The
slice does not skip planning decodes, journal validation, root mutation checks,
checksum refresh on durable publication, or recovery behavior.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, file-format, or durable byte changes.

## Single-File And Lifecycle Impact

No file lifecycle, journal, recovery, lock, sidecar, or embedded lifecycle
change. The resident dirty-buffer page remains checksum-dirty until the
existing publication path refreshes it.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Production
code adds one narrow first-party proof helper. Test-hook builds add one scalar
counter and benchmark row for maintained-root insert fast replacements.

## Tests And Verification Plan

- Add focused storage self-test coverage that stores a maintained root in the
  dirty buffer, replaces it with a one-cell inserted root page, verifies the
  resident bytes decode correctly, and asserts the maintained-root fast
  replacement counter increments.
- Keep existing planned maintained-root insert-position and checksum-deferral
  tests covering writer mutation without maintained-root decode sites.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert benchmark output reports maintained-root insert fast
  replacements for the proven one-cell subset of the `668` checksum-dirty
  `index-root` dirty-buffer replacement path, and any nonmatching root
  replacements continue to use the existing full-page fallback.
- Maintained-root decode sites remain protected and unchanged.
- Dirty-buffer replacement page-family totals, checksum totals, and durable
  publication behavior remain equivalent.
- Storage and embedded storage-engine smoke verification pass.

## Risks

- The proof assumes maintained-root cells are fixed-width and stored
  contiguously from `MYLITE_STORAGE_FORMAT_INDEX_ROOT_PAYLOAD_OFFSET`. If the
  maintained-root format gains variable-width cells or unwritten padding inside
  `used_bytes`, this fast path must fall back to full-page replacement.
- This removes a full-page dirty-buffer copy, not a checksum call. Benchmark
  timing may be noisy even when the structural fast-path counter confirms the
  writer-side replacement work was narrowed.

## Verification Result

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `315.36 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `399.80 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,981,690` bytes (`32.41 MiB`) with `478`
  members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step sampled at `72.171 us/op`. Structural counters stayed
  equivalent: `668` checksum-dirty `index-root` replacements, `8` full-page
  checksum calls, `227,063` zero-tail checksum calls, and `677`
  maintained-root decodes. The new maintained-root insert fast path handled
  `666` one-cell replacements; the remaining `2` root replacements fell back
  to the existing full-page copy path.
