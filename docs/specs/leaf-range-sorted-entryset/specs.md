# Leaf Range Sorted Entryset

## Problem

The prepared-insert profile counter slice showed that the current benchmark
still executes millions of raw-entry order probes during maintained branch
leaf-range redistribution:

- 7,918 raw entry order builds.
- 8,032,627 raw entry order probes.
- 384,427 zero-tail checksum calls.

The previous leaf-range identity-order slice lets the encoder skip allocating
an order array when the entryset is already sorted, but the encoder still has
to rediscover sortedness by walking adjacent entries. Branch leaf-range
redistribution already reads sorted branch leaves in branch child order, then
adds one inserted `(key, row_id)` tuple. It can preserve sorted order while it
constructs the entryset and tell the encoder that no order probe is needed.

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
- `decode_index_leaf_page()` validates each leaf page before its entries are
  appended to the redistribution entryset.
- `decode_index_branch_page()` validates child fences in sorted branch order.
- `insert_raw_index_entry_in_order_to_entryset()` already inserts one raw index
  entry into an existing sorted entryset by `(key, row_id)`.
- `encode_zeroed_index_leaf_page()` already treats `order == NULL` as identity
  order.

## Design

Keep branch leaf-range redistribution entrysets sorted as they are built:

1. Continue reading the selected contiguous branch child leaves in branch order.
2. Replace the final append of the inserted row with
   `insert_raw_index_entry_in_order_to_entryset()`.
3. Extend `prepare_index_leaf_range_pages()` with an internal
   `entries_are_ordered` flag.
4. When that flag is true, encode the range with `order == NULL` and skip
   `build_raw_index_entry_order_if_needed()` entirely.
5. Preserve the existing lazy sort/probe fallback for callers or tests that do
   not prove ordering.

This is deliberately narrower than a checksum redesign. It removes a redundant
rediscovery pass from the branch range writer while leaving the durable page
format and checksum lifecycle untouched.

## Compatibility Impact

No SQL-visible behavior change. Prepared and direct inserts still route through
the same MyLite storage engine for `ENGINE=InnoDB` and supported aliases. The
change only preserves and communicates an existing in-memory ordering invariant.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. The sortedness flag is transient encode-time state.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public API, durable file-format, license, or dependency change. Binary-size
impact is limited to one internal parameter and using an existing sorted insert
helper.

## Test And Verification Plan

- Update storage hook coverage so sorted leaf-range inputs passed as
  known-ordered perform zero raw order builds and zero raw order probes.
- Keep unsorted fallback coverage proving it still builds an order array and
  emits sorted pages.
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

- Branch leaf-range redistribution inserts the new entry in sorted order.
- Ordered range encoding skips raw order builds and raw order probes.
- Unsorted fallback encoding remains available and covered.
- Existing storage, embedded storage-engine, and performance smoke checks pass.

## Verification Result

Verified on 2026-05-28 in the `custom-storage` worktree:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed; no formatting diff.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`:
  passed, `1/1` test in `316.80 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `2/2` tests in `348.90 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. On this VPS run, prepared insert step measured `85.081 us/op`;
  targeted counters were `388` raw entry order builds and `668` raw entry
  order probes, down from the prior profile evidence of `7,918` builds and
  `8,032,627` probes.

## Risks

The ordered fast path relies on branch and leaf decode validation preserving
the sorted branch-leaf invariant. If a future caller cannot prove that
invariant, it must use the fallback path.
