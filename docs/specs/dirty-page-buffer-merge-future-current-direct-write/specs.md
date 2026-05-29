# Dirty Page Buffer Merge Future-Current Direct Write

## Problem

The prepared-insert profile shows that merge-sourced dirty `index-leaf` pages
are still admitted into the parent dirty-page buffer under pressure. The latest
future-page relation counters show the hot rows are not append-buffer resident
and are not past the parent statement's current header page count: they are
future only relative to the parent statement's stable rollback header.

That means the existing protected-existing-page direct-write guard is too
strict for some newly allocated maintained leaves. Replaying every such leaf
through the parent dirty buffer forces parent pressure flushes even though
statement rollback can discard those pages by truncating back to the stable
header page count.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice touches first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and benchmark/doc output.
  No upstream MariaDB source files or handler APIs are changed.
- `mylite_storage_commit_statement()` propagates a child statement's dirty
  current header to the parent before `merge_dirty_page_undos()` and
  `merge_dirty_page_buffer()`.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` previously
  classified every page id greater than or equal to `parent->header.page_count`
  as `future-page`, even when the page id was below
  `parent->current_header.page_count`.
- `mylite_storage_rollback_statement()` restores the stable statement header,
  clears dirty buffers, trims append buffers, and truncates the file to the
  stable current header page count. Newly allocated pages beyond
  `statement->header.page_count` therefore do not need dirty-page undo
  preimages for rollback.
- `flush_statement_append_page_buffer()` and top-level dirty-buffer flushes
  already write newly allocated pages before the header is durably published.
  The new path keeps that same publication ordering.
- Append-buffer-resident pages cannot be direct-written safely through this
  path because a later append-buffer flush could overwrite the direct-written
  image.
- A broad future-current direct-write experiment eliminated dirty leaf pressure
  admissions, but regressed the prepared insert step to `94.432 us/op` on the
  VPS by directly writing growing leaves that still benefit from parent-buffer
  coalescing.

## Design

Extend the child dirty-buffer merge guard with three new outcomes:

- `future-current-header-direct-write`
- `future-append-buffer`
- `future-current-header-partial-leaf`

When the parent dirty-page buffer is full, a merge entry may publish directly
when all of the following are true:

- the entry page id is greater than or equal to `parent->header.page_count`;
- the parent has a current header and the page id is below
  `parent->current_header.page_count`;
- neither the parent nor child append-page buffer contains the page id;
- the entry is an index leaf with `entry_count == index_leaf_entry_capacity()`;
- the parent dirty-page buffer does not already contain the page id; and
- the page size is `MYLITE_STORAGE_FORMAT_PAGE_SIZE`.

The direct write uses the existing dirty-page buffer entry flush helper, so
checksum refresh and raw page write semantics remain shared with buffered
flushes. Existing protected-existing-page direct writes still require a
dirty-page undo preimage in the parent statement chain. Branch pages stay on
the buffered replay path so repeated branch entry-count and fence rewrites
continue to coalesce. Partial future-current leaves remain on the fallback
path because the benchmark shows they are still frequently growing and benefit
from parent dirty-buffer coalescing.

Test-hook future-page relation counters now record any merge entry beyond the
parent stable header, including entries that direct-write under the new
future-current-header guard. This keeps benchmark evidence comparable after the
behavior change.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, or storage-engine routing
changes. `ENGINE=InnoDB` and other routed table behavior continue through the
same MyLite storage path. The change is internal publication timing for
merge-sourced maintained index leaf pages.

## Single-File And Lifecycle Impact

No files are introduced. Durable state remains in the primary `.mylite` file
plus the existing MyLite-owned journal lifecycle. Direct-written future-current
leaves are pages already allocated in the statement current header; rollback
discards them through the existing file truncation to the stable statement
header page count.

## Public API And File Format Impact

No public API or file-format changes. The guard outcome names are test-hook
and benchmark evidence only.

## Binary Size And Dependency Impact

No new dependencies. Production code adds one conservative branch in the merge
direct-write guard and reuses the existing append-buffer containment helper.
Test-hook builds gain three guard outcome names and focused self-test coverage.

## Tests And Verification

- Add storage self-test coverage proving a full dirty `index-leaf` merge entry
  within the parent current header direct-writes under full parent-buffer
  pressure, does not evict a parent dirty-buffer victim, records the new guard
  outcome, records the existing future relation, and is discarded by rollback
  truncation to the stable header page count.
- Preserve existing protected-existing-page direct-write and future-page
  relation tests, including append-buffer fallback and partial-leaf fallback
  outcomes.
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

- Merge-sourced future-current full dirty `index-leaf` entries direct-write
  when the parent dirty-page buffer is full and append buffers do not contain
  the page.
- Append-buffer-resident future pages, branch pages, parent-resident pages,
  partial future-current leaves, non-full parent buffers, and pages past the
  parent current header remain on fallback replay paths.
- Existing rollback semantics truncate direct-written future-current pages
  when the parent statement rolls back.
- Prepared-insert benchmark output shows fewer merge-sourced dirty leaf
  pressure admissions if the workload still matches the observed relation
  profile.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- The path changes publication timing for newly allocated maintained leaves.
  The retained full-leaf policy is deliberately narrower than the broad
  future-current policy that regressed the prepared-insert step through extra
  direct file writes.
- If a future workload keeps the same page in an append buffer and dirty-page
  buffer simultaneously, the guard must continue to choose fallback replay so
  commit-order append flushes cannot overwrite a direct-written image.

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

The final MariaDB smoke archive was `33,973,930` bytes (`32.40 MiB`). The
repeated prepared-insert smoke profiles reported `75.813 us/op`,
`77.155 us/op`, `76.368 us/op`, and a final `78.515 us/op` prepared insert
step. The retained full-leaf policy reported:

- `3,330` dirty `index-leaf` `future-current-header-direct-write` guard rows;
- `113,367` dirty `index-leaf` `future-current-header-partial-leaf` fallback
  rows;
- `81,802` dirty `index-leaf` pressure admissions from dirty-buffer merge;
- `284` `index-branch` pressure admissions, including `140` dirty branch rows;
- `122,388` dirty `index-leaf` future-page relation rows, all
  `within-current-header` with append relation `none`; and
- `36,150` leaf growth fast replacements.
