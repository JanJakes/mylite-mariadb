# Replacement Page Classification Facts

## Problem

The prepared-insert profile records `165,529` dirty-buffer replacement pages:
`34,548` `index-leaf`, `668` `index-root`, and `130,313` `index-branch`
pages. In test-hook builds, `record_dirty_page_buffer_replacement_page()`
classifies the replacement page family, then rechecks the same page to update
leaf fill-band counters and branch-level counters.

The current profile uses this path for every dirty-buffer replacement while the
replacement family, write-site, leaf fill-band, and branch-level rows must stay
unchanged.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  dirty-page replacement counters.
- `record_dirty_page_buffer_replacement_page()` computes the page family with
  `test_dirty_page_buffer_flush_page_family(page)`, records family totals, then
  calls `record_dirty_page_buffer_replacement_leaf_fill_band(page)` and
  `record_dirty_page_buffer_replacement_branch_level(page, checksum_dirty)`.
- `record_dirty_page_buffer_replacement_leaf_fill_band()` computes
  `dirty_page_buffer_index_leaf_occupancy(page)`, which checks leaf identity and
  reads leaf occupancy.
- `record_dirty_page_buffer_replacement_branch_level()` checks branch identity
  before reading the branch level.
- The current prepared-insert profile reports unchanged structural totals:
  `8` full-page checksum calls, `227,063` zero-tail checksum calls, `677`
  maintained-root decodes, `21,031` pressure admissions, `66,144` merge direct
  writes, `87,176` index-leaf dirty refreshes, `31,938` pressure-context
  builds, and `19,053` planned stores.

## Design

Keep the public and non-test storage behavior unchanged. In
`MYLITE_STORAGE_TEST_HOOKS` builds, compute replacement page facts once inside
`record_dirty_page_buffer_replacement_page()`:

- page family for replacement family totals and write-site totals,
- index-leaf occupancy for replacement leaf fill-band totals,
- branch-level classification only when the already-computed family is
  `index-branch`.

Change the internal leaf fill-band and branch-level recorders to consume these
facts rather than rechecking page kind. Keep their existing counter semantics
and invalid-band behavior.

Do not change dirty-buffer replacement policy, fast replacement helpers,
checksum bytes, flush selection, journaling, recovery validation, or maintained
root planning.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format, or
compatibility support status changes. This is test-hook-only accounting work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer replacements, flushes, rollback,
statement commit, and storage file bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the same compiled storage
path and replacement recorder call shape.

## Tests And Verification

- Existing storage self-tests cover dirty-buffer replacement family totals,
  leaf fill-band rows, branch-level rows, branch change classes, and write-site
  rows.
- Verified:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
    passed in `309.94 sec`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
    passed in `330.00 sec`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
    produced a static archive of `33,989,146` bytes with `478` members
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert benchmark reported `74.732 us/op` for the prepared insert
step. Replacement rows stayed unchanged: `34,548` checksum-dirty `index-leaf`
replacement pages, `668` checksum-dirty `index-root` replacement pages,
`130,313` `index-branch` replacement pages with `130,311` checksum-dirty,
leaf fill-band rows of `1,458` `1-24%`, `1,512` `25-49%`, `14,614`
`50-74%`, `16,866` `75-99%`, and `98` full, branch-level rows of `130,313`
level-`1` pages, and branch change rows of `115,753` entry-count-only,
`14,172` entry-count-and-fence, and `386` structural.

The structural counters stayed unchanged: `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `677` maintained-root decodes, `87,178`
dirty-page-flush checksum refreshes, `21,031` pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, and `19,053` planned stores.

## Acceptance Criteria

- Replacement family rows stay `34,548` `index-leaf`, `668` `index-root`, and
  `130,313` `index-branch`.
- Replacement leaf fill-band, branch-level, branch change-class, and write-site
  rows stay unchanged.
- Full-page checksum calls, zero-tail checksum calls, maintained-root decodes,
  pressure admissions, merge direct writes, dirty refreshes, pressure-context
  builds, and planned stores stay unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- Branch-level recording must preserve invalid-band behavior for non-branch
  pages. Gating on the already-computed page family must only skip the same
  non-branch pages the old `is_index_branch_page()` check skipped.
