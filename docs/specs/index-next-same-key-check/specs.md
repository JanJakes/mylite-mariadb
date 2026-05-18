# Index Next Same Key Check

## Problem Statement

MariaDB's default `handler::index_next_same()` calls `index_next()` first, then
compares the materialized row against the requested key. For MyLite's current
cursor shape, that can read and reconstruct the next row payload only to return
end-of-range for unique point lookups. The sorted cursor already has the next
key bytes, so the handler can check the key before materializing the row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::index_next_same()` delegates to
  `index_next()` and then calls `key_cmp_if_same()` against the materialized
  record buffer.
- `mariadb/storage/mylite/ha_mylite.cc::index_next()` materializes the next row
  through `read_index_cursor_row()`.
- MyLite's cursor entries are sorted and contain key offsets, key sizes, and
  row ids, which is enough to compare the next key tuple before reading the row.

## Scope

- Override `ha_mylite::index_next_same()`.
- Compare the next cursor entry's key bytes with the requested key before row
  materialization.
- Materialize the next row only when the cursor key still matches.
- Keep MariaDB key tuple comparison through `mylite_compare_key_tuple()`.

## Non-Goals

- Change range scan semantics outside `index_next_same()`.
- Implement storage-level B-tree navigation.
- Cache materialized rows.
- Add benchmark thresholds or product performance claims.

## Compatibility Impact

The override should preserve MariaDB's `index_next_same()` contract: return the
next row only while the active index key remains equal to the requested key, and
return end-of-file otherwise.

## Single-File And Embedded Lifecycle

No file-format or lifecycle changes. The slice only changes in-memory handler
cursor iteration.

## Test And Verification Plan

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `tools/mylite-perf-baseline`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror --lines=110:118 mariadb/storage/mylite/ha_mylite.h`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror --lines=1629:1650 mariadb/storage/mylite/ha_mylite.cc`
- `git diff --check`

## Acceptance Criteria

- Existing equality, range, prefix-index, FK, generated-index, and
  application-schema coverage remains green.
- The performance baseline reports correct checksums and no point-operation
  regression.

## Implementation Evidence

`ha_mylite::index_next_same()` now compares the next sorted cursor key against
the requested key before calling `read_index_cursor_row()`. When the next key
falls outside the equality range, the handler returns `HA_ERR_END_OF_FILE`
without materializing a row.

Local before/after performance evidence on the same machine, comparing against
the bound-search baseline:

| Operation | Previous us/op | Current us/op |
| --- | ---: | ---: |
| direct primary-key point selects | 2944.680 | 2908.000 |
| prepared primary-key point selects | 2965.680 | 2904.560 |
| direct primary-key updates in one transaction | 9446.990 | 9477.550 |
| prepared primary-key updates in one transaction | 18080.370 | 17778.150 |

## Risks And Open Questions

- This avoids one unnecessary materialization at equality range boundaries, but
  cursor construction is still scan-and-sort until storage-level B-tree
  navigation exists.
