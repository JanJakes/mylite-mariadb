# Deferred Index Row Validation

## Problem Statement

After lazy handler row materialization, durable point lookups still build index
entrysets by reading every referenced row page inside
`mylite_storage_read_index_entries()`. For ordinary indexed reads the storage
entryset only needs live key bytes and row ids; the selected row is already
validated later through `mylite_storage_read_row()` when the handler
materializes it.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_entries()`
  scans append-only index-entry pages, filters table id and row-state entries,
  then calls `validate_live_row()` for each surviving entry before appending it
  to the entryset.
- `packages/mylite-storage/src/storage.c::mylite_storage_index_prefix_exists()`
  uses the same per-candidate row validation before reporting a prefix match.
- `mariadb/storage/mylite/ha_mylite.cc::read_index_cursor_row()` now validates
  and materializes only the selected row id through `mylite_storage_read_row()`
  or `mylite_volatile_read_row()`.
- Row-state pages are the current visibility authority for appended update,
  delete, and truncate tombstones. Header rollback excludes uncommitted tail
  pages from the scan.

## Scope

- Stop reading referenced row pages while collecting durable index entrysets.
- Stop reading referenced row pages in durable prefix-existence probes.
- Keep table-id and row-state filtering in the storage scan.
- Defer row-page and BLOB/TEXT overflow validation to APIs that actually read a
  row payload.
- Preserve existing volatile MEMORY/HEAP behavior.

## Non-Goals

- Implement B-tree or key-directed index lookup.
- Change append-only row-state semantics.
- Change corruption handling for explicit row-read and table-scan APIs.
- Add benchmark thresholds or product performance claims.

## Compatibility Impact

Normal SQL behavior should not change. Supported index entries still filter
deleted and superseded row ids. If a durable index entry references a corrupt or
missing row page, APIs that read the row payload still report the error when
that row is selected; entryset and prefix scans no longer validate unrelated row
payloads eagerly.

## Single-File And Embedded Lifecycle

No file-format or lifecycle changes. The slice changes only which durable pages
are read while collecting index metadata.

## Test And Verification Plan

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `tools/mylite-perf-baseline`
- `/opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror --lines=2748:2762 --lines=2828:2840 packages/mylite-storage/src/storage.c`
- `git diff --check`

## Acceptance Criteria

- Storage unit tests still cover index-entry update/delete/truncate visibility.
- Routed storage-engine smoke remains green, including BLOB/TEXT prefix index
  reads and MEMORY/HEAP volatile index reads.
- The local performance baseline continues to report correct checksums and does
  not regress point-select/update timings.

## Implementation Evidence

Durable index entryset scans and prefix-existence probes now trust the
index-entry table id and row-state tombstone map for visibility. They no longer
call `validate_live_row()` for every surviving entry. Selected rows are still
validated by `mylite_storage_read_row()` when the handler materializes a row.

Local before/after performance evidence on the same machine, comparing against
the lazy handler-row materialization baseline:

| Operation | Previous us/op | Current us/op |
| --- | ---: | ---: |
| direct primary-key point selects | 3718.770 | 3261.320 |
| prepared primary-key point selects | 3638.330 | 3013.490 |
| direct primary-key updates in one transaction | 10786.750 | 9741.190 |
| prepared primary-key updates in one transaction | 18786.840 | 17809.660 |

## Risks And Open Questions

- This is still scan-based index access. It removes avoidable row reads during
  entryset construction, but point lookups still scan append-only index-entry
  pages and sort in memory.
- Corrupt row payloads are now reported by row-read consumers rather than
  unrelated entryset scans. That is intentional for this slice, but future
  integrity-check tooling should provide explicit whole-file validation.
