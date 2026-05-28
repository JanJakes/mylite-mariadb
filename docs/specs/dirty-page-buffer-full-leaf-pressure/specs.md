# Dirty Page Buffer Full Leaf Pressure

## Problem

The prepared-insert dirty-page buffer flush write-site table shows that the
buffer-limit pressure victims are all dirty maintained index leaves written by
`insert_branch_index_leaf_entry`. That means pressure is publishing dirty leaf
pages from the same hot branch-maintenance path that is still admitting and
replacing dirty leaves.

The current pressure selector prefers index leaves over branch/root pages and
prefers clean leaves before dirty leaves. When all candidate leaves are dirty,
it falls back to the first leaf in round-robin order. It does not distinguish a
full leaf, which should no longer receive simple same-leaf inserts, from a
partially filled leaf that may be rewritten again by the next insert.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c` and
  `packages/mylite-storage/tests/storage_test.c`.
- `dirty_page_buffer_pressure_flush_index()` chooses the buffered page to flush
  under `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT`.
- `insert_branch_index_leaf_entry()` rejects leaves whose `entry_count` is at
  capacity and routes full leaves to split, redistribution, or deeper branch
  handling instead of fitting another simple leaf entry.
- Dirty buffered index leaf pages already carry raw leaf metadata in the page
  header. Reading `key_size`, `entry_count`, and `used_bytes` is enough to
  identify a structurally full leaf without decoding entries or refreshing the
  checksum.

## Design

Refine the buffer-limit pressure selector:

- keep the existing first priority for clean index leaves, because flushing a
  clean leaf avoids immediate checksum refresh work;
- when all candidate leaves are dirty, prefer the first structurally full dirty
  index leaf in round-robin order;
- keep the existing first-dirty-leaf fallback when no full dirty leaf is
  present;
- keep the non-leaf fallback unchanged.

The full-leaf check is deliberately local and conservative. It returns true
only for an index leaf whose key size is valid, whose entry count equals leaf
capacity, and whose used byte count matches the exact full-leaf payload size.
Malformed or ambiguous pages stay on the existing fallback path.

## Compatibility Impact

No SQL, public C API, handler API, storage-engine routing, DDL metadata, or
file-format behavior changes. This only changes which buffered maintained-index
page is published first when buffer-limit pressure must flush a dirty page.

## Single-File And Lifecycle Impact

No new files are introduced. Dirty pages still flush to the primary `.mylite`
file through the existing journal-protected write path. Rollback preimages,
nested statement merge, and checksum-dirty handling remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. The binary-size impact is limited to one small helper and
a branch in the dirty-buffer pressure selector.

## Tests And Verification

- Add a storage test-hook case that fills the dirty buffer with branch pages
  plus one partial dirty leaf and one full dirty leaf, triggers pressure, and
  verifies the full dirty leaf is flushed while the partial dirty leaf remains
  buffered.
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

- Clean leaf preference is preserved.
- Dirty full leaves are selected before dirty partial leaves under
  buffer-limit pressure.
- Existing pressure write-site, flush write-site, replacement, checksum, and
  branch writer counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- A full leaf can still be rewritten by split or redistribution paths, so this
  is a pressure heuristic, not a proof that the page is cold forever. The
  policy only applies after clean leaves are unavailable and preserves the
  existing dirty-leaf fallback.
- If a future leaf format changes metadata layout, the helper must be updated
  with that format. It currently validates versioned fixed-width leaf metadata
  before classifying a page as full.
