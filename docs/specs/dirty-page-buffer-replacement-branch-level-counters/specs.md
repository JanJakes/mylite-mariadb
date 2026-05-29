# Dirty Page Buffer Replacement Branch Level Counters

## Problem

The prepared-insert profile now shows dirty-page buffer replacement churn by
page family, write site, and index-leaf fill band. The latest
100,000-iteration run after `d3168a3a` still reports `129,541` index-branch
replacements, including `122,238` checksum-dirty replacements from
`insert_branch_index_leaf_entry`, but the benchmark does not show whether
those branch rewrites are lower level-`1` branch pages or propagated upper
branch pages.

Without branch-level evidence, follow-up work cannot tell whether the next
optimization should focus on lower-branch child metadata rewrites, upper-fence
refresh propagation, or dirty-buffer pressure behavior.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite profiling hooks only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `record_dirty_page_buffer_replacement_page()` already observes every rewrite
  of a page that is already resident in the dirty page buffer, with page
  family, checksum-dirty state, and maintained write site.
- Branch pages carry their maintained-tree level in
  `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_LEVEL_OFFSET`, which can be read from the
  already-buffered page without a checksum-validating decode.

## Design

Add test-hook counters for dirty-page buffer replacement branch pages grouped
by branch level:

- invalid branch metadata;
- level-`1`;
- level-`2`;
- level-`3`;
- level-`4`;
- level-`5+`.

The benchmark prints replacement and checksum-dirty replacement page counts per
branch-level bucket near the existing replacement page-family, leaf fill-band,
and write-site tables. Non-branch replacements remain covered by the existing
family table.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable bytes change. The counters are
test-hook-only and are emitted by the local performance baseline tool.

## Single-File And Lifecycle Impact

No files are introduced. The counters observe existing in-memory dirty-buffer
replacement activity and do not change journal, rollback, flush, or file
lifecycle behavior.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook behavior is unchanged. Test-hook builds add
small fixed counter arrays and accessors.

## Tests And Verification

- Add a storage test-hook case that replaces resident dirty-buffer pages with
  level-`1`, level-`2`, level-`5+`, and invalid branch pages and verifies
  replacement and checksum-dirty branch-level counters.
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

- Replacement branch-level counters reset with prepared-insert profiling
  counters.
- Branch-page replacements are counted by level bucket and checksum-dirty
  state.
- Existing replacement family/write-site, replacement leaf fill, flush,
  pressure, checksum, and leaf-shape counters still report correctly.
- The prepared-insert benchmark prints the new replacement branch-level table.

## Verification Evidence

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step measured `88.047 us/op` on this VPS.
- The benchmark reported all `129,541` branch replacements in the `level-1`
  bucket, with `122,238` checksum-dirty replacements.

## Risks

- Branch-level counters show which branch pages are rewritten in memory, not
  whether a given level is the dominant CPU cost. They are evidence for
  follow-up performance work, not a behavior change by themselves.
