# Scoped Write Journal Finish

## Problem

The prepared update hot path already captures the active MyLite statement while
opening the file for update. It still finishes each row update through
`finish_write_journal(file, filename)`, which rediscovers the active statement
from the filename before returning early for active checkpoints.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `open_existing_file_for_update_scope()` records the active statement in
  `mylite_storage_update_file_scope`.
- `update_row_with_index_entries()` already stores that pointer as
  `active_file_statement` and passes it to begin-journal, header-publication,
  and active-cache helpers.
- `finish_write_journal()` checks `active_statement_for(filename)` before
  deciding whether the recovery journal should be finished immediately.
- In active checkpoints, that check always finds the active statement and
  returns without touching the journal file.

## Design

- Add `finish_write_journal_for_statement()` that accepts the active statement
  pointer already captured by the caller.
- Keep `finish_write_journal()` as the public internal convenience wrapper for
  paths that do not have scoped state; it delegates to the new helper after
  resolving the active statement by filename.
- Use the scoped helper in `update_row_with_index_entries()`, the sampled hot
  row-update path.

## Compatibility Impact

No SQL, C API, storage-engine routing, file-format, or durability behavior
changes. Active statement checkpoints still retain the recovery journal until
checkpoint commit or rollback. Non-checkpoint writes still finish the recovery
journal immediately.

## Single-File And Lifecycle Impact

No file lifecycle changes. The slice only avoids redundant active-statement
lookup when the caller already has the same ownership state.

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

- The hot row-update path no longer rediscovers active statement ownership
  when finishing a write journal.
- Existing journal behavior is unchanged for active and standalone writes.
- Focused and storage-smoke tests pass.
- Prepared-update timing does not regress.
