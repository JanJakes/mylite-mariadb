# Dirty Page Buffer Leaf Insert Replacement Change Counters

## Problem

The first leaf replacement change-class profile after `0b1cc5c5` reported
`3,762` append-only replacements but `62,630` valid `other` leaf replacements.
For prepared inserts, a large part of that `other` bucket is likely interior
single-entry insertion into a sorted leaf payload: the page grows by one cell,
but the old payload is split around the inserted cell rather than remaining a
simple prefix.

The current classifier cannot distinguish that bounded insert shape from
broader leaf rewrites, so it still does not give enough evidence for an
in-place leaf replacement fast path.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook code only:
  `packages/mylite-storage/src/storage.c`. The existing storage test entry
  point and benchmark reporter consume the dynamic slot list without source
  changes.
- Leaf replacement classification already validates leaf key size, entry
  counts, used-byte ranges, stable metadata before `ENTRY_COUNT`, payload
  width, and tail bytes.
- A single interior insert can be identified without decoding SQL values:
  `new_entry_count == old_entry_count + 1`, `new_used_bytes == old_used_bytes
  + cell_size`, tail bytes after the new used range match, and the old payload
  equals the new payload with exactly one cell removed.

## Design

Add a test-hook-only `insert` leaf replacement change class between `append`
and `same-shape`.

The classifier keeps `append` as the fast prefix case. If append does not
match but the page grew by exactly one leaf cell, scan possible insertion cell
positions and classify the replacement as `insert` when the old payload matches
the new payload around that one inserted cell. All non-matching valid leaf
changes remain `other`.

This slice does not change production dirty-buffer replacement behavior.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, durable file format, or persisted bytes change. The new
counter exists only in test-hook builds.

## Single-File And Lifecycle Impact

No files are introduced. Journal, rollback, pressure flush, statement commit,
and dirty-buffer lifecycle behavior remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Production builds are unchanged. Test-hook builds add one
classifier branch over bounded leaf-entry counts.

## Tests And Verification

- Extend the storage test-hook leaf replacement classifier case to cover an
  interior single-entry insert.
- Verify benchmark output reports the new `insert` row.
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

- Prepared-insert benchmark output separates append-only leaf replacements,
  interior single-entry inserts, same-shape rewrites, shrink/refold rewrites,
  and other valid leaf replacements.
- Existing dirty-buffer replacement behavior, branch replacement fast paths,
  flush behavior, rollback behavior, and checksum-dirty semantics remain
  unchanged.

## Risks

- The insert classifier must stay byte-exact and bounded by validated leaf
  entry counts so it cannot read outside the page or misclassify broader
  rewrites as single-entry inserts.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 294.61 seconds.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  built `libmariadbd.a` at 32.40 MiB with `PLUGIN_MYLITE_SE=STATIC`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in 355.78 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported the prepared insert step at `82.174 us/op`, with leaf replacement
  change classes split into `3,762` append, `62,630` insert, and `0` other,
  invalid, identical, same-shape, or shrink replacements.
