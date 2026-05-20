# Borrowed File Scope Close

## Problem

Prepared primary-key updates reuse active MyLite statement files, but some hot
lookup and update paths still finish through `close_existing_file(FILE *)`.
That helper must rediscover whether the `FILE *` belongs to an active write
statement, read statement, read snapshot, transaction-journal snapshot, or an
owned standalone open. In active-statement row-DML loops the caller already has
that scope information, so the rediscovery becomes repeated no-op work.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `open_existing_file_scope()` records whether the returned file is borrowed
  from `active_statement`, `active_read_statement`, or an
  `active_read_snapshot`.
- `open_existing_file_for_update_scope()` records whether the returned file is
  borrowed from `active_statement` or `active_read_statement`.
- `find_indexed_row_payload()` and `update_row_with_index_entries()` already
  keep these scope structs while doing the hot point-read and update work.
- The sampled prepared-update profile still shows no-op
  `close_existing_file()` checks under `find_indexed_row_payload()` and
  `update_row_with_index_entries()`.

## Design

- Add close helpers for `mylite_storage_file_scope` and
  `mylite_storage_update_file_scope`.
- When the scope says the file is borrowed from an active statement/read
  statement, clear the stream error state and return without walking global
  active chains.
- When the scope says the file is an active read snapshot, clear the temporary
  snapshot marker exactly as `close_existing_file()` does.
- When no borrowed marker is present, delegate to `close_existing_file()` so
  transaction-journal snapshots and standalone opens keep their existing close
  semantics.
- Use the helpers only in the sampled hot paths for this slice.

## Compatibility Impact

No SQL, public C API, file-format, storage-engine routing, or durability
semantics change. The helper is a pure ownership fast path over information the
open-scope helper already returned.

## Single-File And Lifecycle Impact

No new durable or transient files are introduced. Borrowed statement-owned
handles continue to close at statement commit/rollback or read-statement end.
Standalone files and transaction-journal snapshot files still close through the
existing `close_existing_file()` path.

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

- Active-statement indexed-row lookup and update close through scope-aware
  helpers.
- Owned files, read snapshots, and transaction-journal snapshots retain their
  previous close behavior.
- The focused storage, embedded, and storage-smoke suites pass.
- Prepared-update performance does not regress.

## Risks

- A helper that treats an owned file as borrowed would leak the handle. The
  design avoids that by delegating whenever the scope lacks an explicit borrowed
  marker.
- The update-scope helper preserves the current read-statement lock behavior;
  changing lock downgrade/release semantics is out of scope for this slice.
