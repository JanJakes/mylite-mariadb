# Row Update Active Cache Threading

## Problem

Prepared primary-key updates inside one transaction now avoid the larger
statement-checkpoint and volatile-snapshot costs, but the storage update path
still repeatedly resolves active statement caches it already identified earlier
in the same call. In the current prepared-update sample, row validation and row
payload replacement still spend time walking active cache sets and comparing
filenames or catalog identity for each row mutation.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB file is changed.
- `update_row_with_index_entries()` already resolves the active file statement
  through `open_existing_file_for_update_scope()` and the outer active cache
  statement through `active_cache_statement_from_statement()`.
- `validate_direct_live_row_in_statement()` immediately looks up the matching
  live-row cache, and then its live/validated marking path may look up or append
  the same cache again.
- `replace_active_row_payload_in_statement()` resolves the active row-payload
  cache by filename, catalog root, catalog generation, and table id after the
  update has already resolved the same statement and table identity.
- The hot benchmark is `tools/mylite-perf-baseline --phase=prepared-updates
  1000 1000000`, which runs `UPDATE perf_rows SET value = value + 1 WHERE
  id = ?` in one transaction over `ENGINE=InnoDB` routed to MyLite.

## Design

- Resolve the active live-row cache at most once for the row-update validation
  path and thread that pointer into the validation and post-update live-row
  replacement helpers.
- Keep existing cache lookup wrappers for storage call sites that still need
  them, while the row-update path uses cache-pointer helpers directly.
- Resolve the active row-payload cache once in `update_row_with_index_entries()`
  after table-id discovery, using the already-resolved active statement as the
  file identity, and pass the cache pointer to the replacement path.
- Do not create a row-payload cache solely for update replacement. If no indexed
  read populated one, the update remains a no-op for that cache as before.
- Preserve all rollback and visibility semantics: cache updates still happen
  only after header publication and journal completion succeed.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or metadata behavior changes.
The optimization only removes redundant first-party cache discovery inside an
already-active update statement.

## Single-File And Lifecycle Impact

No file-format, durable sidecar, journal, or checkpoint lifecycle changes. The
same active checkpoint owns the caches before and after this slice.

## Binary-Size Impact

Negligible. The change adds small helper functions and does not add
dependencies or public symbols.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Prepared point updates no longer need to rediscover the active row-payload
  cache from `replace_active_row_payload_in_statement()`.
- Row validation and post-update live-row replacement reuse the same resolved
  live-row cache pointer when one exists.
- Statement rollback, transaction rollback, active rewrite, and durable
  row/index tests still pass.
- The prepared-update benchmark completes with the expected checksum, and a
  local sample no longer shows the old row-payload replacement lookup wrapper
  in the hot stack.

## Risks And Unresolved Questions

- Cache pointers are valid only while the owning statement cache arrays are not
  cleared or reallocated. This slice keeps pointer use inside one update call
  and only after operations that do not clear the relevant cache sets.
- Broader control-plane costs remain, including per-update table-id lookup and
  journal ownership checks; those should stay separate slices if they still
  appear in profiles.
