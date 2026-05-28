# Leaf Range Identity Order

## Problem

Post-commit prepared-insert profiling on the VPS showed the remaining storage
hot path under maintained branch insert work. A `gprofng` run over
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 200000`
reported `mylite_storage_append_row_with_index_entries()` spending visible time
in `write_branch_index_root_inserts()`,
`redistribute_branch_index_leaf_range_entry()`,
`prepare_index_leaf_range_pages()`, `build_raw_index_entry_order()`, and
`compare_raw_index_entry()`.

`prepare_index_leaf_range_pages()` always allocates and insertion-sorts a raw
entry order before encoding redistributed leaf pages. The general leaf-run
encoder already has an identity-order path through
`build_raw_index_entry_order_if_needed()`, and sorted input can be encoded with
`order == NULL`. Leaf-range redistribution should reuse that path instead of
building an order array when the collected leaf entries plus inserted entry are
already in `(key, row_id)` order.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- First-party branch insertion and redistribution lives in
  `packages/mylite-storage/src/storage.c`, including
  `redistribute_branch_index_leaf_range_entry()` and
  `prepare_index_leaf_range_pages()`.
- `encode_zeroed_index_leaf_page()` already treats `order == NULL` as identity
  order.
- `build_raw_index_entry_order_if_needed()` already probes adjacent raw entries
  and only allocates an order array when the entryset is not sorted.
- Existing identity-order coverage proves `prepare_index_leaf_pages()` avoids
  an order build for sorted entrysets and still sorts unsorted entrysets.

## Design

Change `prepare_index_leaf_range_pages()` to call
`build_raw_index_entry_order_if_needed()` instead of
`build_raw_index_entry_order()`. Keep the existing page distribution and
encoding logic unchanged. When the helper returns `order == NULL`, the existing
leaf-page encoder writes the entryset in identity order; when it returns an
order array, unsorted entrysets keep the existing sorted output semantics.

Add a focused storage test hook that drives `prepare_index_leaf_range_pages()`
with a sorted two-page range and asserts that no raw order array is built. The
same hook also drives an unsorted two-page range and asserts that the order
array is still built and output remains sorted.

## Compatibility Impact

No SQL-visible behavior change. Prepared and direct inserts still route through
the same MyLite storage engine for `ENGINE=InnoDB` and supported aliases. The
change only skips redundant process-local ordering work when the durable leaf
range is already sorted.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, or companion-file behavior
changes. The order array is transient encode-time state and is not persisted.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public API, durable file-format, license, or dependency change. Binary-size
impact is limited to replacing one internal helper call and adding test-hook
coverage.

## Test And Verification Plan

- Add storage hook coverage for sorted and unsorted
  `prepare_index_leaf_range_pages()` inputs.
- Keep storage and routed embedded storage-engine tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Sorted leaf-range entrysets encode without building a raw order array.
- Unsorted leaf-range entrysets still build an order array and encode sorted
  pages.
- Existing storage, embedded storage-engine, and performance smoke checks pass.

## Verification

Run on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  1/1 tests in 305.62 seconds.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; rebuilt `libmysqld/libmariadbd.a` at 32.40 MiB with
  `PLUGIN_MYLITE_SE=STATIC`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, 2/2 tests in 315.01 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step measured 89.833 us/op, bind measured 0.684 us/op,
  reset measured 0.152 us/op, and commit measured 47.126 ms. Storage counters
  showed zero branch leaf-range plan reads, zero branch refold reads, zero
  branch tail overlay scans, zero branch tail overlay scan reads, and two
  packed index tail-append missing-page blockers.

## Risks

Sorted-input probing still compares adjacent entries before it can skip the
order array. Avoiding even that probe requires a stronger sortedness contract at
the redistribution caller and belongs in a later slice if profiling shows this
path remains material.
