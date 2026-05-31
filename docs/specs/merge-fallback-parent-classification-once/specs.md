# Merge Fallback Parent Classification Once

## Problem

Prepared-insert test-hook runs still spend benchmark-path work classifying
dirty-page merge fallback leaves. When a child dirty-buffer leaf cannot direct
write, the test-hook path records both:

- whether the incoming leaf is below the parent dirty-buffer max leaf page id;
  and
- the incoming leaf's tail distance from that same parent max leaf.

Those two counters currently compute the same parent max leaf page id with two
separate scans of the parent dirty buffer. The current profile keeps `21,031`
`future-current-header-partial-leaf` fallback admissions, so the duplicate
test-hook scan is measurable noise in the prepared-insert benchmark.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite test-hook attribution in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  code is involved.
- `merge_dirty_page_buffer()` records fallback origin metadata only in
  `MYLITE_STORAGE_TEST_HOOKS` builds.
- `dirty_page_buffer_merge_fallback_parent_leaf_page_id_rank_for_entry()` and
  `dirty_page_buffer_merge_fallback_parent_leaf_tail_distance_for_entry()`
  each scan the same parent dirty buffer to find the same max leaf page id.
- The recorded counter dimensions are diagnostic only; production storage
  behavior, dirty-buffer replay, pressure selection, direct-write policy, and
  file bytes do not depend on these helper functions.

## Design

Replace the two test-hook helper calls with one parent-leaf classification
helper that scans the parent dirty buffer once and returns both the rank and
tail-distance slots.

Keep the existing slot enums, counter names, benchmark output, and fallback
origin tags unchanged. Do not change production builds, dirty-buffer pressure
policy, direct-write guard outcomes, checksum timing, journal protection, or
file format.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata, transaction semantics, or error-surface
changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change.

## Test And Verification Plan

- Reuse the existing fallback parent-rank/tail-distance self-test coverage,
  which asserts the same benchmark counters.
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

- Merge fallback origin attribution scans the parent dirty buffer once for
  rank plus tail distance.
- Existing fallback parent-rank, tail-distance, rejected-candidate, and
  pressure-victim counters remain structurally unchanged.
- Prepared-insert maintained-root decode and checksum counters remain
  structurally unchanged.

## Risks

- This is a benchmark/test-hook source-path cleanup, not a production storage
  behavior change. It must not be presented as a product semantic or durability
  improvement.

## Verification Results

- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `302.55 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `328.03 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,989,146` bytes and `478` archive members.

The prepared-insert benchmark
(`/tmp/mylite-merge-fallback-parent-classification-once-benchmark.txt`)
reported:

- prepared insert step: `73.834 us/op`;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- maintained-root decodes: `677`;
- `future-current-header-partial-leaf` fallback rows: `21,031`;
- merge pressure-context builds: `31,938`;
- merge pressure-context planned stores: `19,053`; and
- rejected below-tail candidate admissions: `121`.
