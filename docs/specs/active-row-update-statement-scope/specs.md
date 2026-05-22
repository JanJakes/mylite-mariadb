# Active Row Update Statement Scope

## Problem

Prepared primary-key updates already read the target row through an active
storage statement or transaction checkpoint, but the subsequent row mutation
still entered
`update_row_with_index_entries()` through the raw filename update wrapper. In
the pre-slice prepared-update sample, the mutation path still showed
`open_existing_file_for_update_scope()` and
`read_header_from_update_file_scope()` under
`mylite_storage_update_row_with_index_entry_changes()`.

For active statement callers, the storage statement already owns the `FILE *`,
filename, current header, active cache chain, and append buffer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  reads the exact unique target row into `table->record[0]`, then calls
  `ha_mylite::update_row()`.
- `ha_mylite::update_row()` prepares row bytes and index-entry change
  metadata, then calls either
  `mylite_storage_update_row_preserving_index_entries()` or
  `mylite_storage_update_row_with_index_entry_changes()` for durable rows.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  opens an update-file scope, derives active statement/cache/append-buffer
  owners from that scope, reads the current header from the scope, and closes
  the scope at the end.
- For a borrowed active write statement,
  `statement->file`, `statement->filename`, `statement->current_header`,
  `statement->header`, and `statement->parent` already carry the state needed
  by the row-update implementation.
- Raw filename callers still need the existing update-file scope because they
  may need recovery, exclusive locking, active read-statement upgrade, standalone
  open/close, and header publication to page `0`.

## Design

- Add statement-scoped durable row-update APIs for the two handler paths:
  preserving existing index entries and updating with an explicit
  `index_entry_changed` vector.
- Add a storage statement/file-match guard so the handler can safely reuse a
  THD statement or transaction checkpoint when no TLS-active statement is
  directly borrowable.
- Refactor `update_row_with_index_entries()` so the existing filename wrappers
  keep opening and closing the update-file scope, while the new active-statement
  wrappers pass a pre-resolved update context.
- Let the active-statement path use the resolved statement's file, filename,
  current header, active cache owner, and append-buffer owner directly.
- Keep `read_header_from_update_file_scope()` and
  `open_existing_file_for_update_scope()` for raw filename callers and active
  read-statement upgrade paths.
- In `ha_mylite::update_row()`, use the statement-scoped API when a current
  active statement is available for the primary file; otherwise keep the
  existing filename API.

## Affected Subsystems

- MyLite storage durable row-update internals.
- MyLite storage handler update path in `ha_mylite.cc`.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL result, warning, affected-row, duplicate-key, foreign-key, or
transaction behavior should change. The same row validation, duplicate-key,
foreign-key, autoincrement, journal, append/rewrite, rollback, and cache
maintenance helpers remain in use.

## DDL Metadata Routing Impact

No DDL metadata routing change.

## Single-File And Embedded Lifecycle Impact

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change. Raw filename callers keep the recovery and lock/open/close path.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change. The added storage
APIs are first-party internal package APIs used by the MariaDB storage handler.

## Storage-Engine Routing Impact

No routing-policy change.

## Binary-Size Impact

Expected small noise from factoring the row-update body behind a statement
context wrapper.

## License Or Dependency Impact

None.

## Tests And Verification

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/include/mylite/storage.h
  mariadb/storage/mylite/ha_mylite.cc`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, 2/2 tests. `mylite_storage_test` includes direct coverage for the
  statement-scoped row-update APIs and statement/file match guard.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10/10
  tests.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed;
  `libmariadbd.a` was 21,188,112 bytes / 20.21 MiB with 481 members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`: passed; the final
  unsampled run measured bind `0.024 us/op`, step `2.054 us/op`, and reset
  `0.024 us/op`.
- Delayed sample:
  `/tmp/mylite-active-row-update-statement-scope.sample.txt`. The prepared
  update mutation path sampled
  `mylite_storage_update_row_with_index_entry_changes_in_statement()`,
  `update_row_with_index_entries_for_context()`, and
  `rewrite_active_update_pages()`. `open_existing_file_for_update_scope()`
  was only sampled during setup inserts under
  `mylite_storage_append_row_with_index_entries()`, not under the update
  mutation path; `read_header_from_update_file_scope()` was not sampled.

## Acceptance Criteria

- Active handler row updates can call storage through a resolved active
  statement or matching THD checkpoint without entering
  `open_existing_file_for_update_scope()` or
  `read_header_from_update_file_scope()` on the accepted direct-update mutation
  path.
- Raw filename update callers still use the update-file scope helper.
- Existing storage and embedded storage-engine tests pass.

## Risks And Unresolved Questions

- The active path must only accept a borrowed live statement from the current
  active scope. Like the indexed-row active lookup, misuse checks can guard null
  statement/file/filename values but cannot prove an arbitrary stale pointer is
  live.
- Header publication behavior for fallback append paths must remain identical.
  If a row update does not use the active buffered rewrite path, the active
  statement wrapper still has to publish the advanced header through the same
  statement-aware helper.
