# Dirty Undo Direct Buffer Preimage

## Problem

The current prepared-insert profile reports `664` dirty `index-leaf`
undo-capture preimage copies: `654` from branch leaf-range redistribution and
`10` from branch leaf splitting. Those copies are required for rollback when a
writer publishes over a page that is already present in the active dirty-page
buffer chain.

The current undo-capture implementation copies a dirty-buffer page into a stack
scratch page and then immediately copies that scratch page into the dirty undo
list. That intermediate full-page copy is redundant: the dirty-buffer entry is
already the validated rollback preimage source and the undo list owns its own
copy after append.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  code is involved.
- `capture_dirty_page_undo_for_pager_write()` already validates page size,
  header page exclusion, active statement ownership, append-buffer exclusion,
  duplicate undo avoidance, and journal protection before reading the preimage.
- `copy_dirty_page_buffer_undo_preimage()` exists for callers that need a page
  materialized into an output buffer, but undo capture only needs to append the
  preimage bytes into `statement->dirty_page_undos`.
- `append_dirty_page_undo_with_dirty_state()` copies the provided page into the
  owned undo record, preserving rollback lifetime after dirty-buffer entries
  are later discarded or overwritten.

## Design

Inside `capture_dirty_page_undo_for_pager_write()`, scan the active statement
chain for the dirty-buffer preimage directly after journal protection:

- when the page is found in the dirty-buffer chain, record the same test-hook
  copy context and append the undo record directly from `entry->page` with
  `entry->checksum_dirty`;
- when the page is not found in dirty buffers, keep the existing durable
  `read_page_at()` fallback and append a checksum-valid undo record; and
- leave `copy_dirty_page_buffer_undo_preimage()` unchanged for read-style
  callers that need a materialized page buffer.

This removes one intermediate `MYLITE_STORAGE_FORMAT_PAGE_SIZE` stack copy per
dirty-buffer undo capture while preserving the owned undo record, rollback
semantics, journal protection, and test-hook counters.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, storage
engine routing, metadata, file format, or checksum algorithm changes.

## Single-File And Lifecycle Impact

No durable file, journal layout, recovery layout, sidecar, lock, or embedded
lifecycle change. The same undo bytes are owned by the statement undo list, and
rollback still restores dirty preimages through the existing checksum-dirty
restore path.

## Test And Verification Plan

- Reuse the dirty-page undo, dirty-buffer rollback, prevalidated writer
  attribution, storage capability, embedded storage-engine, and prepared-insert
  benchmark coverage.
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

- Dirty-page rollback tests pass.
- Prepared-insert dirty-copy, checksum, maintained-root decode, and dirty
  publication counters remain structurally unchanged.
- The dirty undo-capture write-site table still reports `654` redistribution
  leaf preimages and `10` split leaf preimages, proving the direct append keeps
  attribution intact.
- Wall-clock timing is recorded as local evidence, but correctness and
  structural counters are the gate.

## Risks

- The direct append must not borrow dirty-buffer storage. It must still copy
  into the undo list before the dirty-buffer entry can be discarded.
- Test-hook context restoration must happen before returning from the
  dirty-buffer preimage fast path.

## Verification Results

- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `307.53 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `498.09 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,989,410` bytes and `478` archive members.

The prepared-insert benchmark
(`/tmp/mylite-dirty-undo-direct-buffer-preimage-benchmark.txt`) reported:

- prepared insert step: `73.843 us/op`;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- dirty `index-leaf` refreshes: `87,176`;
- maintained-root decodes: `677`;
- dirty undo-capture copies: `664`, all `index-leaf`;
- undo-capture write sites: `654` dirty leaf copies from
  `redistribute_branch_index_leaf_range_entry` and `10` from
  `split_branch_index_leaf_entry`.
