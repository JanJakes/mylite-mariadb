# Index Cursor Bound Search

## Problem Statement

The handler sorts live index cursor entries, then `mylite_find_index_entry()`
still scans them linearly to position exact, prefix, next, previous, and
last-prefix reads. This keeps current point lookups proportional to the number
of live entries even after row materialization and row validation are deferred.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` sorts entries
  with `mylite_sort_index_entries()` before `index_read_map()` and
  `index_read_idx_map()` call `mylite_find_index_entry()`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_find_index_entry()` currently
  compares entries in order for every `ha_rkey_function`.
- MariaDB passes `ha_rkey_function` values such as `HA_READ_KEY_EXACT`,
  `HA_READ_KEY_OR_NEXT`, `HA_READ_AFTER_KEY`, `HA_READ_KEY_OR_PREV`,
  `HA_READ_BEFORE_KEY`, `HA_READ_PREFIX_LAST`, and
  `HA_READ_PREFIX_LAST_OR_PREV` to storage handlers for index positioning.

## Scope

- Use binary bound searches over the sorted in-memory cursor for supported
  index positioning modes.
- Preserve prefix-length comparisons through the existing
  `mylite_compare_key_tuple()` helper.
- Keep cursor construction, sorting, and row materialization unchanged.

## Non-Goals

- Implement storage-level B-tree navigation.
- Avoid append-only index-entry scans.
- Add new SQL compatibility surfaces.
- Add benchmark thresholds or product performance claims.

## Compatibility Impact

Supported index positioning should return the same rows as before for exact,
prefix, next, previous, and prefix-last reads. Unsupported `ha_rkey_function`
values still return `HA_ERR_UNSUPPORTED`.

## Single-File And Embedded Lifecycle

No file-format or lifecycle changes. The slice only changes in-memory handler
cursor search.

## Test And Verification Plan

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `tools/mylite-perf-baseline`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror --lines=6393:6491 mariadb/storage/mylite/ha_mylite.cc`
- `git diff --check`

## Acceptance Criteria

- Existing storage-engine index, prefix-index, FK, generated-index, and
  application-schema smoke coverage remains green.
- The performance baseline reports correct checksums and no point-operation
  regression.

## Implementation Evidence

`mylite_find_index_entry()` now uses lower-bound and upper-bound searches over
the sorted in-memory cursor for exact, prefix, next, previous, and prefix-last
positioning. The comparison remains delegated to MariaDB's key tuple helper
through `mylite_compare_key_tuple()`.

Local before/after performance evidence on the same machine, comparing against
the deferred index row-validation baseline:

| Operation | Previous us/op | Current us/op |
| --- | ---: | ---: |
| direct primary-key point selects | 3261.320 | 2944.680 |
| prepared primary-key point selects | 3013.490 | 2965.680 |
| direct primary-key updates in one transaction | 9741.190 | 9446.990 |
| prepared primary-key updates in one transaction | 17809.660 | 18080.370 |

## Risks And Open Questions

- This improves handler-side search after cursor construction. It does not
  address the larger remaining cost: scanning append-only index-entry pages and
  sorting them for each cursor build.
