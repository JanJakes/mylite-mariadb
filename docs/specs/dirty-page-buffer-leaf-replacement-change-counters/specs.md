# Dirty Page Buffer Leaf Replacement Change Counters

## Problem

After the branch replacement fast paths, the prepared-insert benchmark still
reports `66,392` dirty-buffer replacement pages in the `index-leaf` family.
The current leaf replacement evidence only reports fill bands, so it shows
where rewritten leaves sit by occupancy but not whether those rewrites are
append-only growth, same-shape payload rewrites, shrink/refold rewrites, or
other structural changes.

Without that classification, a production leaf in-place replacement fast path
would need to guess at the dominant rewrite shape.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- Index leaf pages have fixed metadata offsets for page id, table id, index
  number, key size, entry count, used bytes, checksum, and payload.
- Leaf cells are fixed-width within a page: an 8-byte row id plus the leaf key
  bytes.
- `store_dirty_page_in_buffer_at_pressure_write_site()` can compare the
  resident page and incoming page before overwrite, which is the right point
  to classify replacement shape without changing production behavior.

## Design

Add test-hook-only dirty-buffer replacement change-class counters for
`index-leaf` pages:

- `invalid`: old or new page metadata cannot be interpreted as a valid leaf;
- `identical`: all bytes match except checksum;
- `append`: metadata and existing payload prefix match, while entry count and
  used bytes grow by one or more complete leaf cells;
- `same-shape`: key size, entry count, used bytes, and tail are unchanged, but
  payload bytes differ;
- `shrink`: metadata and new payload prefix match while entry count and used
  bytes shrink by one or more complete leaf cells;
- `other`: valid leaf pages changed in another shape.

The classifier ignores checksum bytes but otherwise compares metadata, payload,
and tail bytes. It does not alter dirty-buffer replacement behavior.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, durable file format, or persisted bytes change. The counters
exist only in test-hook builds.

## Single-File And Lifecycle Impact

No files are introduced. Journal, rollback, pressure flush, statement commit,
and dirty-buffer lifecycle behavior remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Production builds do not carry the test-hook counters.
Test-hook builds add one small classifier and accessor table.

## Tests And Verification

- Add a storage test-hook case covering invalid, identical, append,
  same-shape, shrink, and other leaf replacement classes.
- Expose the counters in the prepared-insert benchmark output.
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

- Prepared-insert benchmark output reports leaf replacement change classes.
- Existing dirty-buffer replacement behavior, branch replacement fast paths,
  flush behavior, rollback behavior, and checksum-dirty semantics remain
  unchanged.
- The resulting profile can identify whether a bounded leaf in-place rewrite
  slice is justified.

## Verification Evidence

VPS verification after implementation:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The post-implementation prepared-insert benchmark reported `64.459 us/op` for
the prepared insert step on this VPS. Leaf replacement change classes were:
`0` invalid, `0` identical, `3,762` append, `0` same-shape, `0` shrink, and
`62,630` other. This means a narrow append-only leaf fast path would cover a
small minority of current replacement churn; the dominant leaf shape needs a
follow-up classifier before mutation work.

## Risks

- The classifier must avoid overfitting to one key size. It should derive leaf
  cell width from the page key-size metadata and reject invalid metadata
  instead of reading past the page.
