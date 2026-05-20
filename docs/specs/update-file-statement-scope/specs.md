# Update File Statement Scope

## Problem

Prepared primary-key updates repeatedly resolve the same active storage
statement from filename or `FILE *` ownership inside one row update. The current
hot path already knows whether the update file belongs to an active write
statement, an active read statement upgraded to exclusive access, or a standalone
file handle, but later journal and header-publication helpers rediscover that
scope.

The sampled prepared-update profile after the indexed-read cache-scope slice
still showed statement-owner lookups under `open_existing_file_for_update()`,
`begin_write_journal()`, and `publish_header()` during
`mylite_storage_update_row_with_index_entry_changes()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Handler source: `mariadb/storage/mylite/ha_mylite.cc::update_row()` prepares
  changed index metadata and calls
  `mylite_storage_update_row_with_index_entry_changes()` for durable rows.
- MyLite storage source: `packages/mylite-storage/src/storage.c`.
- `update_row_with_index_entries()` opens the primary file, derives an active
  file statement from `FILE *`, resolves the outer active cache statement by
  filename, starts the write journal, mutates pages, publishes the header, and
  updates active caches.
- `open_existing_file_for_update()` already distinguishes active write
  statements, active read statements, conflicting owners, and standalone file
  opens.
- `begin_write_journal()` needs the active write statement for the same filename
  to reuse the statement-owned recovery journal.
- `publish_header()` needs the active write statement and active read statement
  for the same `FILE *` to update transient headers without re-encoding page `0`
  in active write scopes.

## Design

- Add a small internal update-file scope that records the opened `FILE *`, the
  matching active write statement when present, and the matching active read
  statement when present.
- Keep `open_existing_file_for_update()` as the public internal wrapper for
  existing call sites, and add a scoped helper for the row-update hot path.
- Add statement-scoped variants for write-journal start and header publication.
- Preserve the existing filename-based wrappers for non-hot callers.
- Do not change table-entry cache lookup in this slice. A previous table-entry
  shortcut regressed the embedded WordPress storage smoke, so that needs
  separate root-cause work.

## Compatibility Impact

No SQL-visible, handler API, public C API, or storage-engine routing behavior
change.

## File Lifecycle

No durable file-format, journal filename, recovery, lock, or companion-file
lifecycle change. The slice only passes already-discovered transient statement
ownership through one update call.

## Embedded Lifecycle And API

No public `libmylite` API or embedded lifetime change. Statement-owned file
handles keep the same close behavior.

## Build, Size, And Dependencies

Small first-party C helper split. No new dependency and no expected material
binary-size impact.

## Test Plan

- Build storage-smoke targets with `cmake --build --preset storage-smoke-dev`.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- The row-update path reuses the active write/read statement scope discovered
  while opening the file.
- Existing storage and embedded storage-engine coverage remains green.
- Prepared-update timing does not materially regress.
- Catalog/table-entry cache behavior remains unchanged.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev`: passed.
- `git diff --check`: passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: passed,
  10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `4.223 us/op` and `4.248 us/op` in consecutive runs.

## Risks And Open Questions

- The change intentionally avoids table-entry shortcutting; catalog cache
  lookup still appears in profiles and needs separate correctness analysis.
- Header publication has read-statement behavior for upgraded read scopes, so
  the scoped helper must preserve that path rather than only optimizing the
  active write-statement case.
