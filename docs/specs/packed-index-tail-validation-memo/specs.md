# Packed Index Tail Validation Memo

## Problem

Prepared insert sampling after the active leaf/branch cache work still shows
`cached_packed_index_entry_page_allows_tail_append()` on the CPU path. The
check is required: a packed index-entry cache can only append to an older page
when every later active append-buffer page is still compatible and no later
page has the same table/index/key-size shape. However, the same cache often
validates the same tail range twice per row: once while predicting append page
counts and again while writing the row.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/storage/mylite/ha_mylite.cc::write_row()` after MariaDB has built
  key images for all changed indexes.
- `packages/mylite-storage/src/storage.c::packed_index_entry_append_page_count()`
  validates cached packed index-entry pages before predicting page growth.
- `packages/mylite-storage/src/storage.c::find_cached_packed_index_entry_append_slot()`
  validates the same cache again before writing.
- `rewrite_active_update_pages()` and buffered undo restore paths can mutate
  active append-buffer pages, so a persistent tail memo needs explicit
  invalidation instead of assuming append-buffer bytes never change.

## Design

- Add statement-local tail-validation metadata to each packed index-entry
  append cache:
  - the highest active header page count already checked for this cache;
  - a statement mutation generation.
- On successful tail validation, remember that pages
  `[cache.page_id + 1, header.page_count)` have no incompatible pages and no
  later same-shape index-entry page.
- On the next validation, skip directly to the first unchecked page when the
  cache generation still matches the statement generation.
- Increment the statement generation when buffered append pages are rewritten
  or restored through undo, forcing the next validation to rescan from the
  cached page's tail.
- Keep cache removal behavior unchanged when validation finds an incompatible
  page, an unbuffered page, or a later same-shape page.

## Non-Goals

- No durable metadata, page format, public API, SQL behavior, or routing change.
- No relaxation of the ordering rule that blocks old-page appends after a later
  same-shape packed index-entry page.
- No change to append-buffer flush size or transaction durability semantics.

## Compatibility And Lifecycle Impact

SQL-visible behavior is unchanged. Routed durable tables, including
`ENGINE=InnoDB`, still use MyLite storage under the embedded profile. The memo
is transient statement state, cleared with the append buffer, and invalidated
when buffered pages can change under it.

## Test And Verification Plan

- Add a test hook that:
  - validates a tail containing a row page and an unrelated index-entry page;
  - proves a second validation scans zero pages;
  - proves explicit invalidation rescans the tail;
  - proves a newly appended same-shape page is still detected.
- Print the packed tail-validation scan page count in the prepared insert
  component benchmark when test hooks are enabled.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Repeated packed index-entry tail checks skip already validated tail pages
  until a buffered-page rewrite invalidates the memo.
- Later same-shape packed index-entry pages still block old-page appends.
- Existing rollback, packed-index, storage, and embedded storage-engine tests
  pass.

## Verification Results

On the local `storage-smoke-dev` build, the repeated
`prepared-insert-components 1000 100000` benchmark reported:

- `5,086` packed index tail-append scan pages;
- `24.686 us/op` for the prepared insert step component;
- `18.366 ms` for prepared insert commit;
- `31,653,888` final bytes;
- `7,728` header pages.

The tail-scan counter is now explicit benchmark evidence; the wall-clock step
timing remains noisy across adjacent local runs.

## Risks

- The memo is only valid for active append-buffer bytes. Missing invalidation
  would risk allowing an old-page append after a tail mutation, so the slice
  invalidates on buffered page replacement, partial undo restore, and active
  index-entry rewrite paths.
