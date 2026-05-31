# Index Leaf Split Ordered Entryset

## Problem

After the tail-clear index leaf encoding slice, the prepared-insert smoke
profile still reports `388` raw entry order builds and `668` raw entry order
probes. The remaining high-confidence insert-side source is
`prepare_index_leaf_split_pages()`, which unconditionally builds a sorted order
array before encoding two replacement leaf pages.

Branch split writers reach that helper after decoding one protected leaf page.
`decode_index_leaf_page()` validates that decoded leaf cells are already sorted
by key and row id. The writer then appends one inserted `(key, row_id)` tuple
and asks the split helper to sort the combined entryset again.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- Prepared inserts enter MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- The affected implementation is first-party storage code in
  `packages/mylite-storage/src/storage.c`.
- `decode_index_leaf_page()` validates leaf checksum, page metadata, row
  references, and sorted `(key, row_id)` order before exposing leaf cells to
  split writers.
- `append_index_leaf_entries_to_entryset()` preserves decoded leaf order when
  copying one leaf page into a transient entryset.
- `insert_raw_index_entry_in_order_to_entryset()` already inserts one raw
  `(key, row_id)` tuple into a sorted entryset while preserving sorted order.
- `encode_zeroed_index_leaf_page()` and `encode_index_leaf_page()` already use
  identity order when the order pointer is `NULL`.

## Design

Extend `prepare_index_leaf_split_pages()` with an internal
`entries_are_ordered` flag:

1. When the flag is true, pass `order == NULL` to the two leaf encoders and
   derive split max fences directly from entryset indexes.
2. When the flag is false, keep a lazy `build_raw_index_entry_order_if_needed()`
   fallback so unsorted test inputs and future unproven callers still encode
   sorted pages.
3. Update branch split writers to insert the new tuple through
   `insert_raw_index_entry_in_order_to_entryset()` after copying the validated
   source leaf into the entryset, then pass `entries_are_ordered = 1`.

This only removes redundant writer-side ordering work after protected leaf
validation. It does not remove any planning, journal, checksum, or recovery
validation gate.

## Compatibility Impact

No SQL-visible behavior change. Inserts still publish the same sorted leaf page
bytes and branch fences for supported MyLite-routed engines.

## Single-File And Lifecycle Impact

No durable file-format, page checksum, journal, lock, recovery, sidecar, or
embedded lifecycle change. The ordered flag is transient writer state within
one statement.

## Public API, Binary-Size, And Dependency Impact

No public API or dependency change. Binary-size impact is limited to a small
internal flag and using an existing insertion helper in split writers.

## Test And Verification Plan

- Add focused storage hook coverage proving:
  - known-ordered split entrysets encode sorted first and second leaves with
    zero raw order builds and zero raw order probes;
  - unordered split entrysets still take the fallback order builder and produce
    correct max fences.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Split writers preserve ordered entrysets before split encoding.
- `prepare_index_leaf_split_pages()` avoids order builds and probes for
  known-ordered entrysets.
- The fallback split encoder still sorts unordered inputs.
- Maintained-root decode/checksum validation sites remain protected.
- Storage and embedded storage-engine smoke verification pass.

## Verification Result

Verified on 2026-05-30 in the `custom-storage` worktree:

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `331.85 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `330.70 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,981,466` byte (`32.41 MiB`) `libmariadbd.a`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. The prepared insert step sampled `73.109 us/op`; full-page checksum
  calls stayed at `8`, zero-tail checksum calls stayed at `227,063`, index-leaf
  page clears stayed at `0`, encoded leaf max-cell reads stayed at `0`, raw
  entry order builds fell from `388` to `2`, and raw entry order probes stayed
  at `668`.

Maintained-root decode sites remained protected validation/read gates:

- `read_index_leaf_run_root`: `1` decode, `1` full checksum;
- `plan_maintained_index_root_inserts`: `674` decodes, `2` full checksum,
  `672` checksum-dirty; and
- `validate_recovery_journal_saved_page`: `2` decodes, `2` full checksum.

## Risks

The fast path depends on the caller proving sorted entryset order. Callers that
merge multiple leaves, delete entries, or otherwise cannot prove sortedness
must keep using the fallback path instead of setting `entries_are_ordered`.
