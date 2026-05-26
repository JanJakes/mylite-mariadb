# Direct Branch Leaf Insert Page

## Problem

After branch-tail overlay scans are cached and advanced, prepared insert samples
show the hot maintained-index path under:

```sh
build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000
```

The dominant first-party work is now
`write_branch_index_root_inserts()` -> `insert_branch_index_leaf_entry()` ->
`prepare_index_leaf_pages()`. For a fitting insert into one existing branch leaf,
the current writer decodes the leaf, copies all leaf entries into a transient
entryset, appends the new entry, sorts that entryset, allocates a fresh page,
encodes it, and then writes one page back.

That generic rebuild is correct, but it is unnecessary for the common
single-leaf maintained insert path. The leaf is already decoded, fixed-width,
sorted, and known to have spare capacity.

## Source Findings

- Base line: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB reaches MyLite durable inserts through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- MyLite durable inserts then call
  `mylite_storage_append_row_with_index_entries()`.
- `packages/mylite-storage/src/storage.c::insert_branch_index_leaf_entry()`
  handles fitting inserts into a single-level branch leaf.
- `decode_index_leaf_page()` already validates the leaf checksum, page id,
  table id, index number, fixed key size, entry count, used bytes, row ids, and
  key ordering before the writer mutates it.
- `refresh_index_branch_child_after_leaf_insert()` reads the max cell from the
  encoded leaf and refreshes the branch child fence and entry count.

## Design

Add a direct leaf-page insert helper for fitting branch-leaf inserts:

- validate the decoded leaf belongs to the target table/index/key size and has
  capacity for one more entry;
- binary-search or linearly find the insertion offset using the same key then
  row-id order as `build_raw_index_entry_order()`;
- shift the encoded cells in the existing page buffer with `memmove()`;
- write the new row id and fixed-width key into the selected cell;
- increment the leaf entry count, refresh used bytes, zero the checksum slot,
  and compute the same `checksum_page_zero_tail()` value used by
  `encode_index_leaf_page()`;
- leave branch fence refresh, dirty-page journaling, and page publication on the
  existing path.

This removes transient entryset allocation, sorting, and whole-page re-encoding
for the fitting single-level branch-leaf insert path. Split, redistribution,
refold, multi-level branch insert, update, and delete maintenance keep their
existing rebuild helpers.

## Scope

- Replace the generic leaf rebuild inside `insert_branch_index_leaf_entry()`
  with a direct in-page insert helper.
- Keep all existing branch planning decisions unchanged.
- Record prepared insert component evidence after implementation.
- Update storage architecture and roadmap notes for the optimized fitting leaf
  rewrite.

## Non-Goals

- No file-format, checksum algorithm, or page-size change.
- No branch split, redistribution, refold, delete, or update algorithm change.
- No leaf-page buffering or generic pager/WAL design.
- No public API, handler API, SQL behavior, or storage-engine routing change.

## Compatibility Impact

No SQL-visible behavior changes. The inserted leaf bytes must remain sorted by
key and row id, and exact, prefix, full-index, and row-materializing reads must
continue to observe the same entries through routed `ENGINE=InnoDB` and explicit
MyLite storage.

## Single-File And Embedded Lifecycle

Durable state remains in the primary `.mylite` file. The same statement or
transaction dirty-page journal protects the existing branch root and leaf pages.
The helper introduces no new companion files or recovery behavior.

## Public API, Storage Routing, Binary Size, And Dependencies

No public API, routing, file-format, dependency, or license impact. Binary-size
impact is limited to one first-party helper.

## Tests And Verification Plan

- Keep existing branch fitting insert, high-key append, interior insert,
  rollback, split, redistribution, and refold storage coverage passing.
- Assert the existing high-key fitting branch insert does not append transient
  index-entryset rows while updating the leaf.
- Run:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 166.80 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed in 30.09 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  reported prepared insert step at 90.562 us/op.

## Acceptance Criteria

- Fitting inserts into a single-level branch leaf preserve sorted key/row-id
  order and update the branch child fence.
- Existing statement rollback and forked stale-journal recovery coverage remain
  valid for the dirty branch root and leaf.
- Prepared insert component timing improves locally or the remaining bottleneck
  is measured and recorded.

## Risks

- A direct page rewrite must exactly preserve the existing leaf byte format and
  checksum value shape. The implementation should use the same constants and
  checksum helper as the generic encoder.
- Duplicate secondary keys require row-id tie-breaking. The insertion search
  must match existing raw entry ordering to avoid corrupting page sort order.
