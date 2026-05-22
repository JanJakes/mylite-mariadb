# Direct Exact Unique Cursor State

## Problem

Prepared primary-key point selects now have a table-free lower bound around
sub-microsecond scalar execution, while routed primary-key point selects remain
an order of magnitude higher. One small handler-side cost remains in the
direct exact unique read path: after the handler has already copied the only
matching row into MariaDB's record buffer, it still allocates or initializes a
one-entry cursor key/entry pair solely so later index-next calls can report end
of file.

For a full, non-null, unique, byte-exact key lookup, the direct read either
finds exactly one row or no row. The post-read cursor does not need to retain
key bytes or an entry array to preserve SQL-visible behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc::index_read_map()` first attempts
  `read_exact_unique_index_row_into()` for exact full-key lookups before
  falling back to materialized cursor construction.
- `read_exact_unique_index_row_into()` applies only when the table is durable,
  has no BLOB fields, the key is supported, the key filter is full length, the
  key is non-nullable and unique, and the key shape passes
  `mylite_key_uses_raw_exact_filter()`.
- `mylite_storage_find_indexed_row_into()` copies the matching row payload
  directly into the handler `buf` and returns the found row id.
- `ha_mylite::index_next()` and `index_next_same()` return end of file before
  reading cursor entries when `index_row_index + 1 >= index_row_count`.
- `ha_mylite::position()` publishes `current_row_id`, not cursor entry bytes.

## Design

- Keep the direct exact unique storage lookup unchanged.
- After a successful direct exact unique read, publish only the minimal handler
  cursor state:
  - `index_row_count = 1`;
  - `index_row_index = 0`;
  - `index_cursor_number = index_number`;
  - `index_cursor_filtered = true`;
  - `current_row_id = row_id`.
- Leave `index_keys` and `index_entries` unset for this direct cursor state.
  The first row is already in MariaDB's record buffer, and later `index_next*`
  calls return EOF from the cursor count before dereferencing entries.
- Keep the not-found cursor state unchanged so repeated range calls still know
  this filtered cursor is empty.
- Keep the materialized cursor fallback unchanged for non-unique, nullable,
  prefix, BLOB, unsupported-key, volatile, and broad ordered reads.

## Compatibility Impact

SQL-visible behavior should not change. The optimization applies only to
full-key exact reads over non-null unique raw byte keys, where at most one row
can match. The same row id is still published for `position()`, and subsequent
index continuation calls still return EOF.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. Routed durable MyLite tables using supported exact
unique keys benefit; volatile MEMORY/HEAP and fallback cursor paths remain
unchanged.

## Binary-Size And Dependency Impact

Small handler code removal. No dependency change.

## Tests And Verification

- Rebuild the MariaDB storage-smoke archive because
  `mariadb/storage/mylite/ha_mylite.cc` changes.
- Rebuild storage-smoke embedded storage-engine and performance targets.
- Run focused embedded storage-engine coverage.
- Run the prepared primary-key point-select benchmark.
- Run formatting and whitespace checks.

## Acceptance Criteria

- Direct exact unique reads no longer initialize the unused one-entry cursor key
  and entry arrays after the row has been copied into `buf`.
- Exact missing-key reads still return `HA_ERR_KEY_NOT_FOUND`.
- Existing storage-engine smoke coverage passes.
- Local benchmark evidence records the routed point-select result.

## Verification Evidence

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - 1/1 test passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - Prepared primary-key point selects: `9.277 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects 10000 1000000`
  - Prepared scalar selects: `0.713 us/op`.
- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`

## Risks And Unresolved Questions

- This relies on MariaDB calling continuation functions for this accepted shape
  only after the current row has already been returned. The existing
  `index_next()` and `index_next_same()` guards make that safe for the one-row
  cursor state.
- This is a small handler cleanup, not the planned navigable index or pager
  work. It removes avoidable cursor bookkeeping from the current hot path.
