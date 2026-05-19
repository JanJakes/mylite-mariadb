# Multi-Page Index Leaf Runs

## Problem

Single-level index leaf roots only publish when all live entries fit in one
page. Larger fixed-width indexes keep falling back to append-log exact scans,
which means SQL-created roots are useful only for small tables and the
performance baseline skips leaf-labelled rows at 1000 rows.

The next bounded storage step is not a full B-tree. It is a multi-page leaf run:
publish sorted fixed-width entries across contiguous leaf pages and let exact
lookups binary-search each page before scanning only the append tail after the
run.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaf()`
  currently calls `prepare_index_leaf_page()` and returns
  `MYLITE_STORAGE_FULL` when the fixed-width entryset cannot fit in one
  `MYLITE_STORAGE_FORMAT_PAGE_SIZE` leaf.
- `packages/mylite-storage/src/storage.c::read_index_leaf_exact_entries()` and
  `read_index_leaf_exact_row_ids()` read one leaf page from root metadata, then
  scan append-log pages after `root_page + 1`.
- `packages/mylite-storage/src/storage_format.h` leaves no next-page pointer in
  the leaf page header, but published rebuild output is append-only and can be
  written as a contiguous run. Existing index-root metadata stores `root_page`
  plus total `entry_count`.
- `docs/specs/published-index-leaf-performance-baseline/specs.md` records that
  the 1000-row benchmark skips leaf-labelled rows under the single-page limit.

## Design

- Replace single-page leaf preparation with a contiguous leaf-run writer:
  - sort the full live fixed-width entryset once;
  - split sorted entries into page-sized chunks;
  - encode each chunk as the existing index leaf page type;
  - publish the first page as `root_page` and total live entries as
    `entry_count`.
- Keep the existing leaf page format and storage format version. The contiguous
  run length is derived while reading by accumulating leaf page entry counts
  until the root metadata `entry_count` is satisfied.
- For empty indexes, still publish one empty leaf page so the root metadata has
  a concrete page to validate and the append-tail scan starts after that page.
- Change exact lookup to use the published run before scanning the append tail.
  The follow-up page-search slice derives the run length from root metadata,
  searches page key ranges, and walks only duplicate-spanning neighbor pages.
  Each visited page keeps the existing binary-search-with-local-duplicate-walk
  behavior.
- Treat a missing or malformed page inside a published run as corruption, not a
  fallback. Fallback is only valid when no root exists.
- Preserve the append-tail overlay: row/index/state pages appended after the
  published run remain authoritative for later mutations.

## Compatibility Impact

No SQL-visible behavior changes. Exact lookup results stay defined by the same
raw key bytes and live row-state filtering as the existing scan fallback.

## Single-File And Lifecycle Impact

Leaf-run pages live in the primary `.mylite` file. Publishing appends one or
more leaf pages and repoints the existing catalog index-root record under the
rollback journal that already protects header/catalog updates. Older leaf pages
remain unreachable until free-space management exists.

## Public API And File-Format Impact

No public API change. The existing `mylite_storage_rebuild_index_leaf()` API
can publish larger fixed-width indexes. No new file-format fields are added.

## Storage-Engine Routing Impact

Durable MyLite-routed tables can publish larger explicit SQL index roots
through the handler integration already in place. MEMORY/HEAP and BLACKHOLE
paths remain excluded.

## Binary-Size And Dependency Impact

No new dependency. Binary impact is limited to chunking, multi-page write/read
helpers, and tests.

## Tests And Verification

- Extend storage unit coverage to publish a fixed-width index with enough rows
  to require multiple leaf pages.
- Verify exact lookup across first, middle, and last leaf-run pages.
- Verify a missing key across a multi-page run returns not found.
- Verify append-tail visibility still works after a multi-page root.
- Run the performance baseline at `1000 1` and confirm the leaf-labelled rows
  no longer skip.
- Run storage tests, storage-engine compatibility harness, formatting checks,
  and `git diff --check`.

## Local Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 1`
  - Published `perf_leaf_rows` index `1` with `1000` entries.
  - Direct/prepared published-leaf secondary exact reads ran instead of
    printing the previous skip note.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

## Acceptance Criteria

- `mylite_storage_rebuild_index_leaf()` can publish fixed-width indexes larger
  than one leaf page without returning `MYLITE_STORAGE_FULL`.
- Exact unique and non-unique lookups use the multi-page run and scan only the
  append tail after the run.
- Single-page and empty-index root behavior remains valid.
- SQL-visible storage-engine behavior remains unchanged.

## Risks

- This is still not a maintained B-tree: rebuilds publish a static sorted run,
  and later mutations use the append-tail overlay.
- Without a next-page pointer, the run must remain contiguous and immutable.
  That is acceptable for rebuild publication but not sufficient for future page
  splits or free-space reuse.
- Exact lookup may still be slower at the API level until row materialization,
  row-state filtering, and maintained tree navigation are improved.
