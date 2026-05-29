# Dirty Page Buffer Flush Leaf Replacement State

## Problem

The current prepared-insert smoke profile reports `85,532` buffer-limit
index-leaf flushes and `85,532` insert-loop dirty-page-flush checksum
refreshes for index leaves. Replacement counters also show `35,690`
checksum-dirty index-leaf replacements in the dirty buffer. The benchmark does
not yet show whether flushed leaves were never rewritten after admission, were
rewritten once, or were rewritten repeatedly before pressure published them.

Without that split, the next checksum slice could target the wrong side of the
hot path: first-admitted dirty leaves, single rewrite leaves, or repeatedly
rewritten leaves.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage and benchmark code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `store_dirty_page_in_buffer()` is the dirty-buffer admission and replacement
  point. Replacements already feed leaf/branch replacement counters.
- `record_dirty_page_buffer_flush_page()` is the shared test-hook flush counter
  path for buffer-limit pressure, statement commit, and test-hook flushes.
- The dirty-page buffer entry already carries test-hook-only write-site
  metadata, so replacement-state metadata can remain outside production builds.

## Design

Add a test-hook-only replacement counter to each dirty-page buffer entry:

- initialize it to `0` on first admission and pressure-slot reuse;
- increment it when `store_dirty_page_in_buffer()` updates an existing entry,
  saturating at `UINT_MAX`; and
- classify flushed index leaves as `never-replaced`, `replaced-once`, or
  `replaced-multiple`.

Expose source-by-state counters through existing test-hook accessors and print
them in the prepared-insert component benchmark near the other dirty-page flush
leaf tables.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new field and counters exist only when
`MYLITE_STORAGE_TEST_HOOKS` is enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change flush order, dirty-page
publication, journal protection, rollback, nested statement merge, or checksum
refresh behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds are unchanged. Test-hook builds gain one
small per-entry counter and one counter table.

## Tests And Verification

- Add storage test-hook coverage for flushed leaves with zero, one, and
  multiple in-buffer replacements.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Evidence

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `323.06 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with `32.40 MiB` `libmariadbd.a`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `329.21 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed with a `80.985 us/op` prepared insert step.

The benchmark reported `85,532` buffer-limit index-leaf flushes. The new
replacement-state table split those leaves into `78,921` `never-replaced`,
`4,289` `replaced-once`, and `2,322` `replaced-multiple` victims. That means
the remaining dirty leaf flush checksum work is dominated by first-admitted
dirty leaves rather than leaves repeatedly rewritten in the dirty buffer.

## Acceptance Criteria

- Test-hook counters report flushed index leaves by replacement state and flush
  source.
- Prepared-insert benchmark output includes the new table.
- Existing pressure, flush, rank, fill-band, matrix, replacement, and checksum
  counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- The counter is per-entry transient state. It must reset when a pressure slot
  is reused or when a fresh entry is appended, otherwise a reused slot could
  falsely classify an unrelated page as rewritten.
