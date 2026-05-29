# Dirty Page Buffer Branch Replacement Change Counters

## Problem

Replacement branch-level counters show the remaining prepared-insert branch
rewrite churn is entirely in level-`1` branch pages: the latest 100,000-iteration
run reported `129,541` level-`1` branch replacements, including `122,238`
checksum-dirty replacements. The benchmark still does not show what changed in
those replacement branch pages.

Without old-versus-new branch change evidence, follow-up work cannot decide
whether to focus on deferring page-owned entry-count updates, avoiding child
fence rewrites, or handling structural child-list changes.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite profiling hooks only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `store_dirty_page_in_buffer_at_pressure_write_site()` sees both the existing
  resident dirty-buffer page and the replacement page before overwriting the
  resident buffer slot.
- Branch pages have fixed metadata offsets and fixed-width child cells. Their
  entry-count field, checksum field, child page ids, and child max fence values
  can be compared without a checksum-validating decode.

## Design

Add test-hook counters for branch replacements grouped by old-versus-new
change class:

- invalid comparison input;
- no non-checksum byte change;
- entry-count only;
- child-fence only;
- entry-count and child-fence only;
- structural or other branch bytes.

The comparison ignores the branch checksum field because dirty-buffer branch
pages intentionally defer checksum refresh. It treats branch child-count,
used-bytes, child page-id, metadata, tail-byte, or unclassified payload changes
as structural. It treats child max-row-id or max-key changes as child-fence
changes.

The benchmark prints the change-class table next to the replacement branch
level table. Existing replacement family, level, leaf fill-band, and write-site
tables remain unchanged.

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
a small fixed counter array and branch-page comparison helper.

## Tests And Verification

- Add a storage test-hook case that replaces resident dirty-buffer branch pages
  with entry-count-only, child-fence-only, entry-count-plus-fence, structural,
  and invalid branch changes and verifies the change-class counters.
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

- Branch replacement change counters reset with prepared-insert profiling
  counters.
- Existing branch replacements are classified before the dirty-buffer slot is
  overwritten.
- Existing replacement family/write-site, replacement leaf fill, replacement
  branch level, flush, pressure, and checksum counters still report correctly.
- The prepared-insert benchmark prints the new replacement branch change-class
  table.

## Verification Evidence

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step measured `64.721 us/op` on this VPS.
- The benchmark reported `115,619` entry-count-only branch replacements and
  `13,922` entry-count-plus-fence branch replacements, with `0` structural
  branch replacements.

## Risks

- Change classes are conservative metadata comparisons, not semantic proof that
  a branch rewrite can be skipped. Any future optimization still needs a
  source-backed design and storage tests.
