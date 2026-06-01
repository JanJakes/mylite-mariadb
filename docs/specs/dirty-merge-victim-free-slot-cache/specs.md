# Dirty Merge Victim Free-Slot Cache

## Problem

The prepared-insert storage profile is now at the protected validation floor
for maintained-root decodes and full-page checksum calls, but the dirty page
buffer still does avoidable metadata parsing while deciding whether a future
index-leaf page can be direct-written during child-to-parent dirty-buffer
merge.

The broad-victim guard already selects a flush victim through the dirty page
buffer pressure context. It then recomputes the victim leaf free-slot count from
page bytes with `dirty_page_buffer_page_free_slots()`. Dirty-buffer entries
already carry validated index-leaf fill facts when the page was admitted or
replaced, and other merge paths already use those facts through
`dirty_page_buffer_entry_index_leaf_fill()`.

## Source Findings

- Target base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c`.
- The hot guard is
  `dirty_page_buffer_merge_broad_victim_guard_outcome()` and
  `init_dirty_page_buffer_merge_broad_victim_guard_context()`.
- `init_dirty_page_buffer_merge_broad_victim_guard_context()` currently calls
  `dirty_page_buffer_page_free_slots()` on the selected victim page bytes.
- `dirty_page_buffer_entry_index_leaf_fill()` first returns cached
  `has_index_leaf_fill` facts from the dirty-buffer entry and only falls back
  to parsing page bytes when no entry facts exist.
- The existing incoming-page helper
  `dirty_page_buffer_merge_entry_leaf_free_slots()` already uses those entry
  facts, but it treats full leaves as valid zero-free-slot leaves for incoming
  direct-write classification. The victim helper must preserve the previous
  broad-victim behavior where full or invalid victim leaves do not provide
  victim free-slot facts.

## Design

Add a narrow victim helper that:

- requires an index-leaf dirty-buffer entry,
- reads entry count and capacity through
  `dirty_page_buffer_entry_index_leaf_fill()`,
- returns no victim free-slot facts when the cached or decoded leaf is full or
  invalid, matching the previous page-byte helper behavior,
- reports a test-hook counter when cached entry facts satisfied the request.

Replace only the broad-victim guard's victim free-slot read with the new helper.
Do not change incoming-page classification, dirty-buffer selection policy,
checksum refresh timing, protected page validation, or journal validation.

## Affected Subsystems

- MyLite storage dirty page buffer merge guard.
- MyLite storage test-hook performance counters.
- Storage performance baseline reporting.

No MariaDB handler, SQL parser, optimizer, public C API, storage-engine routing,
DDL metadata, or wire-protocol behavior changes.

## Compatibility Impact

There is no user-visible SQL or API behavior change. The slice only removes a
redundant internal parser pass when the dirty-buffer entry already has the same
validated leaf-fill facts. MySQL/MariaDB compatibility evidence is unchanged.

## Single-File And Lifecycle Impact

No file-format, durable sidecar, recovery, or lifecycle behavior changes. The
helper reads transient in-memory dirty-buffer metadata produced from the same
page bytes already owned by the active statement.

## Public API, File Format, Routing, And Dependencies

- Public API impact: none.
- File-format impact: none.
- Storage-engine routing impact: none.
- Wire-protocol impact: none.
- Binary-size impact: limited to one small static helper and a test-hook
  counter.
- License/dependency impact: none.

## Test And Verification Plan

- Add focused storage self-test coverage that proves a dirty index-leaf buffer
  entry can answer victim free-slot reads from cached fill facts.
- Extend `tools/mylite_perf_baseline.c` to report cached victim free-slot reads
  in the prepared-insert dirty-buffer pressure-context section.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - storage-smoke build, storage-smoke tests, MariaDB static smoke build
  - prepared-insert component benchmark

## Acceptance Criteria

- The broad-victim guard no longer calls a page-byte-only victim free-slot
  parser when cached dirty-buffer entry fill facts are available.
- The prepared-insert benchmark reports a nonzero cached victim free-slot read
  count for the current broad-victim workload.
- Protected-page validation, maintained-root planning validation, recovery
  journal validation, and checksum publication semantics are unchanged.
- Verification passes and the work is committed atomically.

## Risks And Unresolved Questions

- This slice removes metadata parsing only for dirty-buffer entries that already
  carry valid fill facts; entries without those facts still fall back through
  `dirty_page_buffer_entry_index_leaf_fill()`.
- It does not reduce checksum calls by itself. Its expected benefit is reduced
  dirty-buffer merge guard metadata work in the prepared-insert hot path.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `327.68 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `332.95 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,997,714` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed under unrelated high host load (`15.59`, `16.98`, `17.12`). The
  sampled prepared-insert step was `77.776 us/op`.

The comparable prepared-insert benchmark reported `13,004` cached victim
free-slot reads and kept the structural counters unchanged: `8` full-page
checksum calls, `127,063` zero-tail checksum calls, `5` protected
maintained-root decodes, `21,031` dirty leaf pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, and `19,053` planned stores.
