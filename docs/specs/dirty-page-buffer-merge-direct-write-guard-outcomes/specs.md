# Dirty Page Buffer Merge Direct-Write Guard Outcomes

## Problem

The leaf-only dirty-buffer merge direct-write path is intentionally
conservative. The current prepared-insert profile reports zero merge direct
writes, but the benchmark output does not explain whether child merge pages are
blocked because the parent buffer is not full, the page is not an existing
leaf, the page is already parent-resident, or rollback protection is missing.

Without that breakdown, the next pressure slice would have to infer why the
hot merge-sourced leaf admissions stay on the fallback path.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- `merge_dirty_page_buffer()` is the single place where child dirty-buffer
  entries are replayed into the parent statement.
- The direct-write path is safe only for existing index-leaf pages that are not
  already resident in the parent dirty buffer and already have a dirty-page
  undo preimage in the parent chain.
- Branch pages should remain on the buffered replay path so branch
  entry-count and fence rewrites continue coalescing in memory.

## Design

Classify every child dirty-buffer merge entry with one guard outcome before
choosing direct write or fallback replay:

- `direct-write`
- `invalid-statement`
- `unsupported-page-size`
- `parent-not-full`
- `header-page`
- `future-page`
- `non-leaf`
- `parent-resident`
- `missing-undo`

Use the same classification for the actual direct-write decision so the
benchmark counters and production behavior stay aligned. In test-hook builds,
record guard outcomes by page family and checksum-dirty state. Print only
nonzero guard outcome rows in the prepared-insert benchmark.

## Compatibility Impact

No SQL syntax, public C API, handler API, storage-engine routing, metadata, or
file-format changes. Production behavior is unchanged except for factoring the
existing direct-write guard into an outcome helper used by the same decision.

## Single-File And Lifecycle Impact

No files are introduced. Durable state remains in the `.mylite` file plus the
existing MyLite-owned journal lifecycle. The counters are test-hook-only and do
not affect file format or sidecar behavior.

## Binary Size And Dependency Impact

No new dependencies. Production code gains an internal guard outcome enum and
helper. Test-hook builds gain counters and benchmark output.

## Tests And Verification

- Extend direct-write storage test coverage to assert the `direct-write` guard
  outcome counter for a protected existing merge leaf.
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

VPS verification after implementation:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert smoke profile reported a `74.438 us/op` prepared insert
step. The new guard table reported:

- `future-page`: `116,672` dirty `index-leaf` entries;
- `parent-not-full`: `5,716` dirty `index-leaf`, `668` clean `index-root`,
  and `6,756` `index-branch` entries, of which `4,561` were dirty; and
- `non-leaf`: `123,171` `index-branch` entries, of which `117,827` were
  dirty.

There were no current-workload `missing-undo`, `parent-resident`, or
`direct-write` guard rows. That shows the current merge-leaf direct-write
blocker is mostly new/future page ids rather than missing rollback protection
on existing leaves.

## Acceptance Criteria

- Merge direct-write guard outcome counters report nonzero rows in the
  prepared-insert benchmark.
- The direct-write decision uses the same guard outcome that the counters
  report.
- Existing direct-write rollback coverage still passes.
- Storage and embedded storage-engine smoke tests pass.
