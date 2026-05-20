# Active Row Payload Cache Window

## Problem

The 10,000-row prepared update benchmark still samples heavily under
`read_row_page()`, `decode_row_page_metadata()`, and `checksum_page()` while
materializing indexed rows inside an active transaction. The existing active
row-payload cache works for small working sets, but it keeps only 4096 row
payload entries. A 10k-row loop therefore repeatedly clears the cache before it
can cover one full replacement generation, so later update passes reread and
rechecksum row pages that were already materialized.

The cache window needs to cover larger small-row update sets without allowing
large BLOB/TEXT rows to consume unbounded transient memory.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` reads the
  matching row through the handler index cursor before calling
  `handler::ha_update_row()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` builds a
  MyLite index cursor for exact primary-key updates.
- `packages/mylite-storage/src/storage.c::find_indexed_row_payload()` resolves
  the active checkpoint, exact-index row id, and then calls
  `read_indexed_row_payload_from_open_file()`.
- `read_indexed_row_payload_from_open_file()` first probes the active
  row-payload cache, but `store_active_row_payload()` clears the statement
  cache once `MYLITE_STORAGE_ACTIVE_ROW_PAYLOAD_ENTRY_LIMIT` is reached.
- Local sampling of
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 10000 1000000`
  after the append-buffer widening still showed row-page checksum validation
  dominating the active indexed-row materialization path.

## Design

- Increase the active row-payload entry limit from 4096 to 32768 so ordinary
  small-row active transactions can keep a full 10k-row working set resident.
- Add byte accounting to the shared row-payload cache entries.
- Bound active row-payload caches by row bytes as well as entry count:
  - skip caching a single row whose payload exceeds 16 MiB;
  - clear and recreate the active table cache before admitting a row that would
    push that table cache above 16 MiB;
  - remove an existing cached row if replacing it would push the active table
    cache above the same byte budget.
- Leave durable row-payload cache entry limits unchanged. Durable caches share
  the byte accounting field, but their policy remains the existing smaller
  read-cache window.

The byte budget keeps the larger entry window targeted at small records, which
matches the prepared update benchmark and common OLTP row shapes.

## Affected Subsystems

- First-party MyLite storage active row-payload cache.
- Handler-driven indexed row materialization inside active durable
  transactions.
- Local storage performance baseline.

## Compatibility Impact

No SQL, public C API, handler contract, optimizer, or file-format behavior
changes. This is a transient cache policy change; misses continue through the
existing validated row-page read path.

## Single-File And Lifecycle Impact

No durable file-format, journal, WAL, lock, or companion-file change. Cached
payloads remain process memory owned by active statement or transaction
checkpoints and are cleared on rollback, truncate, catalog invalidation, cache
rotation, or statement cleanup.

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
- Compare the prepared-update benchmark against the 10k-row post-append-buffer
  baseline around 10 us/op.
- Optionally sample the same benchmark again to confirm row-page checksum
  validation is no longer the dominant active row-payload miss path.

## Acceptance Criteria

- The active row-payload cache can retain at least a 10k-row small-record
  working set inside one active transaction.
- Large row payloads remain bounded by the active byte budget.
- Existing active row-payload replacement, savepoint rollback, delete,
  durable-cache, and embedded storage-engine tests pass.
- The 10k-row prepared-update benchmark improves or the profile shows that
  active row-payload cache misses no longer dominate the hot path.

## Verification Results

- `git diff --check`
- `git clang-format --diff`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 tests passed.
- Before this slice,
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 10000 1000000`
  measured prepared primary-key updates at `9.990 us/op`, checksum
  `51138894`.
- With the larger byte-bounded active row-payload cache,
  `tools/mylite-perf-baseline --phase=prepared-updates 10000 1000000`
  measured prepared primary-key updates at `3.867 us/op`, checksum
  `51138894`.

## Risks And Unresolved Questions

- This is still a transient cache, not a pager. It reduces repeated active row
  materialization but does not replace the planned navigable index, WAL, and
  dirty-page design.
- The 16 MiB budget is deliberately conservative for one active table cache. A
  future configurable memory profile may expose smaller limits for constrained
  embedded targets.
