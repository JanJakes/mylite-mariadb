# Leaf Replacement Classification Single Pass

## Problem

The prepared-insert profile now shows no remaining maintained-root writer copy
hot spot. The largest remaining storage counters are dirty `index-leaf`
publication (`87,176` dirty refreshes) and the `34,548` leaf growth
replacements produced by `insert_branch_index_leaf_entry`.

A direct current-dirty leaf writer probe showed that the single-leaf insert
writer is not where those leaf replacements occur: the benchmark reported `0`
current-dirty leaf writer hits while the same `34,548` leaf growth
replacements still appeared. The replacement evidence is emitted from the
dirty-buffer replacement/merge path. In test-hook builds that path classifies
the same old/new leaf pair twice: once for merge fallback replacement
counters, and once for replacement leaf-change counters.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected implementation is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source changes are needed.
- `store_dirty_page_in_buffer()` handles replacement of an already-resident
  dirty-buffer entry. Under `MYLITE_STORAGE_TEST_HOOKS`, it records merge
  fallback replacement class and replacement leaf-change class before applying
  the fast replacement helpers.
- Both leaf replacement counters derive from
  `dirty_page_buffer_replacement_leaf_change_class(old_page, new_page)`.
  Calling the classifier twice is redundant for leaf replacements and does not
  change production storage semantics.

## Design

In the replacement branch of `store_dirty_page_in_buffer()`, compute the leaf
replacement change class once when the incoming page is an index leaf, then
feed that class to both test-hook counter families. Keep branch replacement
classification unchanged, keep all production replacement helpers unchanged,
and keep the dirty-buffer replacement count and checksum-dirty behavior
unchanged.

The slice does not change dirty-page publication policy, direct-write guard
predicates, journal validation, checksum refresh behavior, file-format bytes,
or parent/child dirty-buffer ownership.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, transaction semantics, or error surface changes.

## Single-File And Lifecycle Impact

No file lifecycle, sidecar, lock, journal format, recovery format, or embedded
startup/teardown change.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Binary-size
impact is expected to be negligible and is checked by the static MariaDB smoke
build.

## Tests And Verification Plan

- Keep existing storage self-tests for dirty-buffer replacement leaf-change and
  merge fallback counters; they should pass with identical counter values.
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

- Storage and embedded storage-engine smoke verification pass.
- Prepared-insert benchmark counters remain structurally equivalent:
  maintained-root decodes stay at `677`, full-page checksum calls stay at `8`,
  dirty leaf publication counts do not increase, and leaf growth replacement
  counts remain explainable.
- The sampled prepared-insert step does not regress materially.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `313.30 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `332.31 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,983,346` bytes and `478` archive members.
- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.

The final storage-smoke prepared-insert profile
(`/tmp/mylite-leaf-classification-single-pass-benchmark.txt`) reported:

- prepared insert step: `67.983 us/op`;
- maintained-root decodes: `677`;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- dirty `index-leaf` refreshes: `87,176`;
- dirty-page publication refreshes: `21,031` buffer-limit `index-leaf`, `1`
  statement-commit `index-leaf`, `2` statement-commit `index-branch`, and
  `66,144` merge-direct-write `index-leaf`;
- dirty-buffer leaf replacements: `34,548`, split into `4,793` append-only
  and `29,755` interior single-entry insert changes; and
- leaf growth fast replacements matched that same `4,793` append plus
  `29,755` insert split.

Those counters preserve the previous structural shape. The slice only removes
duplicate test-hook leaf change classification before the same counter fanout.

## Risks

- This is a bounded test-hook hot-path reduction, not a dirty leaf publication
  policy change. It should not be presented as reducing the production dirty
  checksum refresh floor.
