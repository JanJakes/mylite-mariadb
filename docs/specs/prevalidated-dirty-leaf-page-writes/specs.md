# Prevalidated Dirty Leaf Page Writes

## Problem

The current prepared-insert profile still routes hot maintained leaf-page
writes through the generic maintained-index writer. That writer must support
leaf, root, and branch pages, so after dirty-buffer admission or direct
fallback it publishes both active leaf-cache and branch-cache metadata.

The remaining production leaf call sites already know they are writing a
validated table-index leaf page. `insert_branch_index_leaf_entry()` is the hot
case: the current profile attributes all `21,031` dirty leaf pressure
admissions and the simple branch-insert leaf rewrite stream to that caller.
The branch-cache publication probe is redundant for those leaf pages.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  code is involved.
- `insert_index_leaf_entry_into_page()` leaves simple branch-insert leaf pages
  checksum-dirty for the maintained writer, while deeper branch insert paths
  pass either checksummed replacement leaves or direct split leaves through the
  same generic maintained writer.
- `pager_write_buffered_maintained_index_page()` preserves dirty-page undo,
  dirty-buffer buffering, checksum-dirty direct fallback, active page-cache
  publication, and stale dirty-buffer discard, but it has to probe both leaf
  and branch cache publication paths.
- Leaf-only callers can keep that writer contract while publishing only the
  active leaf cache.

## Design

Add a leaf-specific maintained writer for prevalidated table-index leaf pages:

- accept the same `buffer_existing_page` and `checksum_dirty` choices used by
  the generic maintained writer;
- buffer existing leaf pages through the dirty-page buffer with the same undo
  capture and checksum-dirty state;
- keep direct fallback writes, dirty checksum refresh, stale dirty-buffer
  discard, and test-hook write-site attribution;
- publish only active leaf-cache metadata after buffered or direct writes; and
- use it for the known branch-insert maintained leaf writes that currently
  remain on the generic maintained writer.

Do not change planning validation, recovery-journal validation, dirty-buffer
pressure policy, checksum timing, page bytes, journal format, or file format.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata, transaction semantics, or error-surface
changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change. The same leaf page bytes are written with the same dirty-page undo and
dirty-buffer semantics.

## Test And Verification Plan

- Extend the dirty undo write-site self-test to cover the buffered
  prevalidated dirty leaf writer.
- Reuse storage and storage-smoke tests that cover branch inserts, deeper
  branch paths, split leaves, dirty-buffer rollback, recovery, and active leaf
  page caches.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Known maintained leaf call sites use the leaf-specific writer.
- Dirty-page undo capture and test-hook write-site attribution work for the
  leaf-specific writer.
- Prepared-insert maintained-root decode and checksum counters remain
  structurally unchanged.
- Branch and leaf writer decode counters remain `0`.

## Risks

- The writer must only be used for table-index leaf pages. Misrouting a root,
  branch, row, catalog, or append-buffer-only page through it could skip a
  needed cache publication path.
- The expected impact is source-path overhead reduction, not a checksum-count
  reduction; wall-clock samples may remain host-noise sensitive.

## Verification Results

- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `318.27 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `335.01 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,989,146` bytes and `478` archive members.

The prepared-insert benchmark
(`/tmp/mylite-prevalidated-dirty-leaf-page-writes-benchmark.txt`) ran under
heavy unrelated host load, so the wall-clock sample is not used as a timing
claim. Structural counters remained stable:

- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- maintained-root decodes: `677`;
- branch insert writer branch decodes: `0`;
- branch insert writer leaf decodes: `0`;
- dirty leaf pressure admissions stayed `21,031`, all attributed to
  `insert_branch_index_leaf_entry`; and
- dirty leaf replacements stayed `34,548`, all attributed to
  `insert_branch_index_leaf_entry`.
