# Packed Index Tail Row Page Advance

## Problem

The prepared insert component benchmark on the VPS still reports
`packed index tail-append scan pages = 5086` after the packed tail-validation
memo. The remaining scans come from newly appended active-buffer pages that the
writer already knows are compatible with packed index-entry cache reuse,
especially packed row pages appended between two index-entry cache pages.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::cached_packed_index_entry_page_allows_tail_append()`
  treats row pages and other-shape index-entry pages as compatible tail pages.
- `write_packed_inline_insert_pages()` appends packed row pages before writing
  index-entry pages, so it can safely advance already-validated packed
  index-entry tail memos across that new row page.
- `write_packed_index_entry_pages()` knows when a newly appended index-entry
  page has a different table/index/key-size shape from existing caches.
- Existing invalidation through
  `invalidate_packed_index_entry_append_tail_checks()` still covers buffered
  page rewrites and undo restores.

## Design

- Add a statement-local helper that advances packed index-entry append-cache
  `tail_checked_page_count` across a just-appended compatible page.
- Only advance caches whose memo generation is current and whose checked range
  already reaches the new page id. This never skips an unchecked gap.
- When appending a packed row page, advance every packed index-entry cache.
- When appending a new index-entry page, initialize the new same-shape cache as
  checked through its own page and advance only other-shape caches.
- Treat a memo checked beyond the supplied header page count as valid for that
  older header. The current write can append and memoize pages before a later
  validation in the same write path receives the post-write header.
- Keep same-shape pages, maintained root/leaf/branch pages, unknown pages, and
  unbuffered gaps as validation blockers.

## Compatibility Impact

No SQL, public API, file format, storage-engine routing, or single-file
lifecycle behavior changes. The slice only tightens transient active-statement
memo maintenance for the existing packed index-entry append path.

## Scope And Implications

- Affected subsystem: MyLite first-party storage active append-buffer index
  cache maintenance.
- DDL metadata routing, wire protocol, MariaDB handler contracts, and durable
  page formats: unchanged.
- Dependency, license, and binary-size impact: no new dependencies; small
  first-party C helper only.
- Risk: advancing a stale memo could hide a blocker, so the helper requires the
  current generation and a contiguous already-checked prefix before it advances.

## Test And Verification Plan

- Extend packed tail-validation hook coverage so a cached index-entry page
  advances across a newly appended row page and the next validation scans zero
  pages, including when validation still receives the pre-write header.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Packed index-entry tail memos advance across compatible writer-appended row
  pages without scanning those pages on the next validation.
- Same-shape index-entry pages still block appending to older cached pages.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; `packed index tail-append scan pages` dropped from the VPS baseline
  of `5086` to `17`, with final bytes still `31653888` and header page count
  still `7728`.
