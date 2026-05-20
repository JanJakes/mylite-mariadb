# Active Payload Validation Proof

## Problem

After widening the active row-payload cache, the prepared-update hot path still
spends visible time marking exact-index hits in the separate active live-row
validation cache. For ordinary indexed updates, `find_indexed_row_payload()`
has already returned a payload from the active row-payload cache or has just
stored the validated payload there. Marking the same row id into both the live
and payload-validated row-id sets adds duplicate hash lookups before
`update_row_with_index_entries()` validates the same row id again.

The row-payload cache can serve as the validation proof for rows it owns.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` reads the
  current row through the handler before dispatching to `ha_update_row()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` calls
  `mylite_storage_find_indexed_row_reuse()` for exact indexed reads.
- `packages/mylite-storage/src/storage.c::read_indexed_row_payload_from_open_file()`
  returns active cached payloads before falling back to row-page reads.
- `find_indexed_row_payload()` then marks the row as active validated-live even
  when the active row-payload cache already proves the payload for the active
  checkpoint.
- `validate_direct_live_row_in_statement_cache()` only consults the live-row
  validation cache before falling back to row-page validation.

## Design

- Make `store_active_row_payload()` report whether the active cache retained
  the payload.
- Have `read_indexed_row_payload_from_open_file()` report when the returned row
  came from, or was stored into, the active row-payload cache.
- Skip the separate active validated-live mark after indexed payload reads when
  the active row-payload cache retained the row.
- Teach `validate_direct_live_row_in_statement_cache()` to accept an active
  row-payload cache entry for the same `(catalog_root_page, catalog_generation,
  table_id, row_id)` as a validated live-row proof.
- Keep the existing live-row validation fallback for rows that are not cached,
  including oversized payloads that the byte budget deliberately skips.

## Affected Subsystems

- First-party MyLite storage active row-payload cache.
- Active live-row validation cache.
- Handler-driven indexed update/delete validation.

## Compatibility Impact

No SQL, public C API, handler contract, optimizer, storage-engine routing, or
file-format behavior changes. This only changes which transient cache proves
that a row payload was already validated inside the active checkpoint.

## Single-File And Lifecycle Impact

No durable file-format, journal, WAL, lock, or companion-file change. The proof
is transient process memory and follows the same row-payload cache invalidation
rules for rollback, truncate, catalog mutation, delete, and statement cleanup.

## Storage-Engine Routing Impact

The optimization applies to durable MyLite-routed tables, including tables
requested as `MYLITE`, `InnoDB`, `MyISAM`, `Aria`, and omitted/default engines
that resolve to MyLite. `MEMORY` / `HEAP` volatile rows do not use this durable
row-payload path.

## Binary-Size And Dependency Impact

Small first-party C change. No dependency and no expected binary-size impact
beyond normal code-size noise.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 10000 1000000`
- Reuse the active row-payload cache tests, especially indexed read followed by
  update, savepoint rollback, delete, and the large-window corruption guard.
- Sample the prepared-update benchmark when useful to confirm live-row marking
  is no longer a prominent hot path.
- Current verification on 2026-05-20:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
    includes `test_active_row_payload_cache_validates_update()`.
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 passed.
  - `tools/mylite-perf-baseline --phase=prepared-updates 10000 1000000`:
    prepared primary-key updates measured 3.785 us/op with checksum 51138894.

## Acceptance Criteria

- Indexed-row reads that are covered by the active row-payload cache no longer
  need a separate active validated-live mark.
- Update/delete validation accepts active row-payload cache entries as proof for
  the same active checkpoint view.
- Oversized or uncached payloads still use the existing live-row validation
  fallback.
- Existing storage and embedded storage-engine tests pass.

## Risks And Unresolved Questions

- A stale row-payload cache entry would now also be a stale validation proof.
  This is acceptable only because existing update/delete/truncate/catalog and
  rollback paths already maintain or clear active row-payload caches.
- This reduces duplicate validation bookkeeping. It does not remove MariaDB
  prepared-statement planning overhead or the remaining buffered-page undo work.
