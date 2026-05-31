# Prevalidated Branch Page Writes

## Problem

The current prepared-insert profile reports no remaining branch writer decodes,
but it still sends hot maintained branch-page writes through the generic
maintained writer. That path must support any maintained index page family, so
after a dirty-buffer store or direct fallback write it probes both active
index-leaf and active index-branch cache publication paths.

The hottest replacement table attributes `122,388` dirty `index-branch`
replacements to `insert_branch_index_leaf_entry()` and `7,537` to
`redistribute_branch_index_leaf_range_entry()`. Those callers already know the
page image is a validated table-index branch page assembled in the same writer.
The leaf-cache publication probe is therefore redundant for those branch pages.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  code is involved.
- `pager_write_maintained_insert_page()` routes existing maintained index pages
  through `pager_write_buffered_maintained_index_page()`, which preserves
  dirty-page undo capture, dirty-buffer storage, fallback durable writes,
  active page-cache publication, and stale dirty-buffer discard.
- `store_active_index_leaf_page_from_pager_write()` and
  `store_active_index_branch_page_from_pager_write()` both read page-family
  metadata without checksum validation. The generic writer must call both;
  branch-only callers only need the branch publication path.
- Branch insert and redistribution writers construct or refresh branch pages
  from decoded branch metadata and known child-fence state before writing them.

## Design

Add a branch-specific maintained writer that mirrors the existing maintained
writer contract while publishing only the active branch cache:

- buffer existing branch pages through the dirty-page buffer with the same
  undo-capture and checksum-dirty state;
- keep the same direct-write fallback and checksum refresh when a page cannot
  be buffered;
- keep stale dirty-buffer discard on successful direct fallback writes;
- preserve test-hook write-site attribution; and
- use it for known branch-page writes in the branch insert and leaf-range
  redistribution paths.

Do not change branch/root planning validation, recovery-journal validation,
checksum timing, page bytes, dirty-buffer replacement policy, or file format.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata, transaction semantics, or error-surface
changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change. The same branch page bytes are written with the same dirty-page undo
and dirty-buffer semantics.

## Test And Verification Plan

- Extend the dirty undo write-site self-test to cover a prevalidated branch
  writer call next to the generic and prevalidated leaf writers.
- Reuse storage and storage-smoke tests that cover branch insert,
  redistribution, split, rollback, recovery, and active branch page caches.
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

- Known branch-page call sites use the branch-specific writer.
- Dirty-page undo capture and test-hook write-site attribution work for the
  branch writer.
- Prepared-insert maintained-root decode and checksum counters remain
  structurally unchanged.
- Branch writer decode counters remain `0`.

## Risks

- The branch-specific writer must only be used for branch pages. Misrouting a
  leaf, root, row, or catalog page through it could skip a needed cache
  publication path.
- The expected benchmark timing impact is source-path overhead reduction, not a
  checksum-count reduction, so wall-clock samples remain host-noise sensitive.

## Verification Results

- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `314.84 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `334.67 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,988,754` bytes and `478` archive members.

The prepared-insert benchmark
(`/tmp/mylite-prevalidated-branch-page-writes-benchmark.txt`) reported:

- prepared insert step: `73.322 us/op`;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- maintained-root decodes: `677`;
- branch writer decodes: `0`;
- dirty branch replacements stayed attributed to `insert_branch_index_leaf_entry`
  (`122,388`), `redistribute_branch_index_leaf_range_entry` (`7,537`), and
  `split_branch_index_leaf_entry` (`386`).
