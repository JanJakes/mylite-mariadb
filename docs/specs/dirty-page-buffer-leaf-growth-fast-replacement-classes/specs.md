# Dirty Page Buffer Leaf Growth Fast Replacement Classes

## Problem

The prepared-insert benchmark currently reports one combined `leaf growth fast
replacements` counter. The underlying fast path already handles both
tail-append growth and interior single-cell inserts, while the broader dirty
buffer replacement leaf-change table reports all replacements before the fast
path decides whether it can mutate the resident slot in place.

Without class-specific fast-path counts, follow-up insert hot-path work cannot
tell whether the byte-proven fast path is covering append and insert
replacement shapes proportionally, or whether a future optimization should
target a class still falling back to full page copy.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite test-hook observability only:
  `packages/mylite-storage/src/storage.c` and
  `tools/mylite_perf_baseline.c`; the existing storage test wrapper in
  `packages/mylite-storage/tests/storage_test.c` continues to invoke the
  focused internal storage self-test.
- `dirty_page_buffer_replacement_leaf_change_class()` classifies dirty-buffer
  index-leaf replacements into invalid, identical, append, insert, same-shape,
  shrink, and other classes before the resident dirty-buffer slot is
  overwritten.
- `replace_dirty_page_buffer_leaf_single_insert()` proves the narrower
  single-cell growth shape and mutates the resident page in place. Its existing
  `insert_index == entry_count` case is an append; lower insert indexes are
  interior single-entry inserts.
- `mylite_storage_test_dirty_page_buffer_fast_replaces_leaf_growth()` already
  verifies an interior insert fast replacement and a same-shape non-fast
  replacement.

## Design

Add test-hook-only counters keyed by the existing leaf replacement change-class
slots for successful leaf-growth fast replacements. Increment the existing
total counter unchanged, and increment either:

- `append` when the inserted fixed-width cell lands at the previous
  `entry_count`; or
- `insert` when the cell lands before the previous tail.

Other leaf-change classes remain valid zero rows for reconciliation with the
shared slot-name table. The benchmark prints a new table beside the existing
replacement leaf-change output. Extend the focused storage self-test to cover
both an interior insert fast replacement and a tail-append fast replacement,
then confirm a same-shape rewrite still avoids the fast counter.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable bytes change. The slice only adds
test-hook counters and local benchmark output.

## Single-File And Lifecycle Impact

No files are introduced. Journal protection, rollback, pressure flush,
statement commit, dirty-buffer pressure policy, and embedded lifecycle behavior
remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Non-test-hook behavior is unchanged. Test-hook builds add
a small fixed counter array and one accessor used by the benchmark.

## Tests And Verification

- Extend `mylite_storage_test_dirty_page_buffer_fast_replaces_leaf_growth()` to
  assert:
  - an interior single-entry leaf insert increments the total fast counter and
    the `insert` fast-class counter;
  - a tail append increments the total fast counter and the `append`
    fast-class counter; and
  - a same-shape leaf replacement increments no fast-class counters.
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

- Prepared-insert profiling resets the class-specific fast replacement counters
  with the existing fast replacement total.
- Successful leaf-growth fast replacements are counted by append versus
  interior insert class.
- Existing replacement leaf-change counters, dirty-buffer replacement behavior,
  rollback behavior, checksum-dirty semantics, and branch fast paths remain
  unchanged.
- The prepared-insert benchmark prints the new class-specific fast replacement
  table.

## Risks

- These counters classify only successful fast replacements, not all leaf
  replacements. The existing replacement leaf-change table remains the source
  for total replacement shape counts.
- Append versus insert is derived from the proven insert position inside the
  fast path. If future leaf growth fast paths cover broader shapes, they should
  add explicit class accounting rather than reusing this narrow counter
  silently.

## Verification Evidence

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 292.44 seconds.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  rebuilt `libmariadbd.a` at 32.40 MiB.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in 316.77 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported the prepared insert step at `81.147 us/op`, `33,583` leaf growth
  fast replacements, `4,769` append fast replacements, `28,814` insert fast
  replacements, and zero fast replacements for the invalid, identical,
  same-shape, shrink, and other leaf classes.
