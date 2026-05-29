# Dirty Page Buffer Merge Pressure Direct Write

## Problem

Prepared-insert pressure evidence now shows that the hot buffer-limit pressure
path is not direct pager admission. All `85,532` pressure admissions in the
current smoke profile arrive while child dirty buffers are merged into the
parent, and all of those incoming child entries were first-admitted entries in
the child buffer. The current merge path still treats each incoming child page
like a normal parent dirty-buffer admission: when the parent buffer is full, it
flushes a parent victim and then installs the child page into the freed slot.

That churn publishes a parent page only to make room for an incoming leaf that
does not need parent-buffer coalescing. A narrower policy can keep the parent
buffer hot by publishing a protected existing merge leaf directly when rollback
protection is already present.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage and tests in
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `mylite_storage_commit_statement()` merges child dirty-page undo preimages
  into the parent before it calls `merge_dirty_page_buffer()`.
- `merge_dirty_page_buffer()` sees each child dirty-buffer entry before replay
  into the parent.
- `store_dirty_page_in_buffer()` currently flushes one parent dirty-buffer
  victim when the parent buffer is at `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT`
  and the incoming page is not already resident.
- `write_dirty_page_buffer_entry()` already refreshes deferred dirty checksums
  and writes a dirty-buffer entry to its durable page id.
- `restore_dirty_page_undos()` restores directly published dirty pages during
  statement rollback from the parent dirty-page undo list.
- Branch pages remain on the existing parent-buffer replay path. The
  prepared-insert profile is dominated by same-page branch replacements, so
  direct branch publication would lose parent-buffer coalescing.

## Design

Add a direct-write path inside child dirty-buffer merge:

- only consider child index-leaf entries when the parent dirty-page buffer is
  already at the protected-page limit;
- skip the path when the incoming page already has a parent dirty-buffer entry,
  because that is a normal replacement/coalescing case;
- require a valid parent file, `MYLITE_STORAGE_FORMAT_PAGE_SIZE`, an existing
  page id below the parent header page count, and a parent-chain dirty-page
  undo preimage for the page;
- publish the incoming child entry directly with the same checksum refresh and
  raw page write semantics as dirty-buffer flush;
- leave the parent dirty buffer unchanged, preserving hot parent pages instead
  of evicting a victim to admit the incoming merge page; and
- add test-hook counters reporting merge direct-write pages by page family and
  checksum-dirty state.

The fallback remains the existing `store_dirty_page_in_buffer()` replay path
for unprotected pages, pages already resident in the parent, non-full parent
buffers, branch pages, unsupported page sizes, and allocation or I/O failures.

## Compatibility Impact

No SQL syntax, public C API, handler API, storage-engine routing, metadata, or
file-format changes. The behavior changes internal publication timing only for
protected existing index-leaf pages during nested-statement dirty-buffer merge.
Rollback and crash-safety remain guarded by the existing dirty-page undo and
journal protection requirements.

## Single-File And Lifecycle Impact

No files are introduced. Durable state remains in the `.mylite` file plus the
existing MyLite-owned journal lifecycle. The direct-write path does not create
sidecars, change header/catalog publication, change append-buffer semantics,
or bypass rollback restore.

## Binary Size And Dependency Impact

No new dependencies. Production code gains one small merge helper and one
guard helper; test-hook builds gain counters and benchmark output.

## Tests And Verification

- Add storage test-hook coverage proving that a protected incoming child leaf
  is direct-written when parent merge pressure would otherwise evict a parent
  victim:
  - parent dirty buffer remains full and does not contain the incoming page;
  - buffer-limit flush and pressure admission counters remain zero;
  - merge direct-write counters report the incoming page family and dirty
    state;
  - the file contains the direct-written page; and
  - restoring parent dirty-page undos restores the original page image.
- Preserve existing merge admission tests for fallback/write-site behavior.
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

The prepared-insert smoke profile reported a `82.166 us/op` prepared insert
step. The current workload had no protected existing merge leaves eligible for
direct write: merge direct-write counters were `0` for all page families, and
pressure admissions remained `85,257` dirty `index-leaf` pages plus `275`
`index-branch` pages through the existing fallback path. Branch replacement
coalescing remained active with `129,541` branch replacements, including
`115,619` entry-count-only and `13,922` entry-count-plus-fence replacements.

## Acceptance Criteria

- Protected merge-sourced existing index-leaf pages can publish directly
  without evicting a parent dirty-buffer victim.
- Existing rollback restore can undo the direct-written page.
- Existing pressure fallback behavior remains for parent-resident incoming
  pages, branch pages, and unprotected inputs.
- Prepared-insert benchmark output reports merge direct-write counters.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Direct writes keep parent dirty-buffer pages resident longer, so the final
  commit may flush a different buffered set. The benchmark must report whether
  this improves or regresses the prepared-insert step before the slice is kept.
