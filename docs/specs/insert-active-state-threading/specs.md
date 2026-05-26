# Insert Active State Threading

## Problem

Prepared insert profiling after the branch-snapshot dirty-buffer work showed
that the ordinary insert path still rediscovered active storage state several
times per row. `mylite_storage_append_row_with_index_entries()` already opens a
scoped update file and receives the active statement for that file, but later
insert steps re-walk the active statement chain when reserving inline append
pages and when maintaining active live-row caches.

Those repeated lookups are not the largest remaining cost; larger long-running
insert samples are still dominated by append-buffer flush writes once the
transient buffer fills. They are, however, unnecessary work on every successful
prepared insert and fit the current roadmap's hot-path cleanup stage.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` calls the first-party storage append
  path from handler row writes after the MyLite handlerton has opened the
  statement or transaction checkpoint.
- `packages/mylite-storage/src/storage.c::open_existing_file_for_update_scope()`
  returns the active statement associated with the update file.
- `mylite_storage_append_row_with_index_entries()` already derives the active
  cache statement from that active file statement before catalog and index-root
  planning.
- `write_inline_insert_pages()` previously called
  `reserve_append_pages_at_raw()`, which rediscovered the active statement and
  append-buffer owner from `FILE *`.
- Successful insert cleanup previously called
  `mark_active_validated_live_row()` and `append_active_live_row_id()`, which
  rediscovered active file or filename cache owners despite the caller already
  having those statements.

## Design

- Derive the active cache statement from the update-file active statement, and
  refresh active/append-buffer ownership after journal setup so the insert fast
  path sees the same checkpoint state the generic helper would discover.
- Thread the active file statement and append-buffer owner into
  `write_inline_insert_pages()` and reserve append pages without another
  active-chain walk.
- Use the existing in-statement live-row validation helper for newly inserted
  row ids.
- Replace the filename-rediscovering active live-row-id cache append helper
  with an in-statement helper for the remaining insert caller.
- Leave append-buffer capacity, flush timing, page encoding, and durable bytes
  unchanged.

## Affected Subsystems

- First-party MyLite storage row insert path.
- Active checkpoint append buffering.
- Active live-row validation and live-row-id cache maintenance.

## Compatibility Impact

No SQL-visible behavior change. `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`,
and omitted/default routed tables continue through the same MyLite storage
handler path.

## Single-File And Lifecycle Impact

No durable file-format or companion-file change. The change only reuses
process-local statement objects that were already active for the same update
scope.

## Public API And File-Format Impact

No public API or durable file-format impact.

## Binary-Size Impact

Small first-party helper split only. No new dependency.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`

## Acceptance Criteria

- Inline insert append-page reservation does not rediscover active statement
  ownership from `FILE *` when the caller already has the active statement.
- Successful insert live-row cache maintenance does not rediscover the same
  active owners by file or filename.
- Existing storage and embedded storage-engine tests pass.
- The prepared insert component benchmark does not regress locally.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`:
  prepared insert step component measured `23.355 us/op`; this is within the
  local noise band of the pre-slice `22.722 us/op` sample.
- `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step component measured `64.780 us/op` and commit measured
  `1903.781 ms`; this preserved the existing append-buffer behavior after an
  early version of the slice accidentally derived the append-buffer owner too
  soon and fell back to direct step-time writes.

## Risks And Unresolved Questions

- This removes repeated lookup overhead but does not reduce write volume. A
  packed row/page format and fuller pager/WAL design are still required for
  SQLite-like sustained insert throughput.
- The active statement objects must continue to refer to the same update file
  scope. The existing `open_existing_file_for_update_scope()` ownership model
  provides that guarantee for this slice.
