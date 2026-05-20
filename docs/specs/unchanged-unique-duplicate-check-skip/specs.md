# Unchanged Unique Duplicate Check Skip

## Problem

`ha_mylite::update_row()` already serializes old and new MariaDB key images and
builds a per-index changed-key vector before publishing a durable MyLite row
update. That vector is currently only used to omit redundant replacement
index-entry pages. Duplicate-key checking still loops over all unique keys,
including stable primary and unique keys whose serialized key image cannot
introduce a new duplicate.

The local routed-storage update benchmark changes `value` while selecting rows
by primary key. After the direct append-buffer rewrite slice, the sampled
profile still showed `mylite_check_duplicate_keys()` as a remaining handler
cost. The benchmark's primary key is unchanged for every update, so those exact
duplicate probes are avoidable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` calls
  `mylite_prepare_index_entry_changes()` before duplicate-key checking and
  durable storage publication.
- `mylite_prepare_index_entry_changes()` uses MariaDB `key_copy()` over the
  old row buffer and compares that with the new serialized key image.
- `mylite_check_duplicate_keys()` and
  `mylite_check_volatile_duplicate_keys()` iterate over every unique key and
  either call exact storage lookup helpers for raw full-key filters or scan the
  index entryset for collation-aware keys.
- `ha_mylite::write_row()` and foreign-key action helpers do not have an old
  row/new row changed-key vector at their existing duplicate-check call sites,
  so they must retain the current all-key duplicate checks.

## Design

- Add an optional changed-key vector parameter to durable and volatile
  duplicate-key checking.
- When the vector is present, skip duplicate-key probes for unchanged keys.
  An update that leaves a unique key's serialized image unchanged cannot
  introduce a new duplicate for that key.
- Keep insert and foreign-key action duplicate checks passing `NULL`, preserving
  the existing all-key behavior for those paths.
- Keep changed unique keys, nullable unique-key handling, raw exact lookup, and
  collation-aware fallback scans unchanged.
- Do not change storage APIs or durable index format. This is a handler
  preflight optimization.

## Affected Subsystems

- MariaDB MyLite handler update path.
- Durable and volatile duplicate-key preflight checks.
- Storage-smoke update performance baseline.

## Compatibility Impact

SQL behavior does not change. Updates that leave unique key images unchanged
continue to succeed or fail based on other changed keys and constraints.
Updates that change a unique key still run the same duplicate check and produce
the same duplicate-key diagnostics.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, transaction, or lifecycle change.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small handler-only first-party change. No new dependency.

## Tests And Verification

- Add routed storage-engine coverage for an update that changes only a
  non-unique indexed column while primary and unique key images stay stable.
- Keep a paired duplicate-key update assertion proving changed unique keys
  still reject duplicates.
- Run the embedded storage-engine test, full storage-smoke CTest gate, update
  performance baseline, `git diff --check`, and `git clang-format --diff` on
  touched C/C++ files.

Verification results:

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  rebuilt the MariaDB embedded storage-smoke archive with the handler change.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 1000000`
  reported direct primary-key updates at `12.869 us/op` and prepared
  primary-key updates at `6.122 us/op`.
- A sampled one-million-update run reported direct primary-key updates at
  `13.026 us/op` and prepared primary-key updates at `6.113 us/op`.
  `mylite_check_duplicate_keys()` dropped to low single-digit sampled hot-path
  presence, while `checksum_page_zero_tail()` and `capture_buffered_page_undo()`
  remained visible.
- `git diff --check`
- `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc packages/libmylite/tests/embedded_storage_engine_test.c`

## Acceptance Criteria

- Durable routed updates pass the changed-key vector into duplicate-key
  checking.
- Unchanged unique keys are skipped only when a changed-key vector exists.
- Inserts, foreign-key action paths, nullable unique-key handling, and changed
  unique keys retain existing duplicate-key behavior.
- Storage and embedded storage-engine tests remain green.

## Risks And Open Questions

- This does not remove duplicate checks for changed unique keys or collation
  fallback paths. Maintained navigable unique indexes remain necessary for
  broader SQLite-like performance.
