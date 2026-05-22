# Handler Row Id Scratch

## Problem

`ha_mylite::materialize_index_cursor_rows()` allocates a temporary row-id array
for every non-unique index cursor it materializes. Hot exact secondary-index
queries rebuild those cursors repeatedly, so the allocation churn sits on the
SQL read path even though the handler object can safely reuse the same scratch
array across cursor builds.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` builds durable
  index cursors and calls `materialize_index_cursor_rows()` when a cursor has
  materializable non-BLOB rows.
- `mariadb/storage/mylite/ha_mylite.cc::materialize_index_cursor_rows()`
  currently allocates, fills, passes, and frees a row-id array on every cursor.
- `ha_mylite` already keeps reusable row scratch for the exact unique
  fixed-row path through `index_row_scratch`.

## Proposed Design

- Add a handler-owned `index_row_id_scratch` buffer and capacity.
- Grow that buffer on demand in `materialize_index_cursor_rows()` and reuse it
  across later cursor builds on the same handler.
- Keep cursor ownership unchanged: row payloads, offsets, and sizes still come
  from `mylite_storage_read_indexed_rows()` and are released by
  `clear_index_cursor()`.
- Free the row-id scratch with the existing handler scratch cleanup.
- Expose focused secondary-select phases in the local performance harness so
  secondary read-path changes can be measured without running the full
  benchmark suite.

## Compatibility Impact

No SQL behavior, storage routing, public API, or metadata behavior changes.
The cursor materializes the same row ids in the same order.

## Single-File And Lifecycle Impact

No durable file, lock, recovery, journal, or companion-file change. The new
buffer is process-local handler memory.

## Public API And File Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

Durable MyLite-routed tables benefit through existing index cursor paths for
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted engine, and
`ENGINE=MYLITE`. Runtime-volatile `MEMORY` / `HEAP` paths are unchanged.

## Binary-Size Impact

Tiny first-party handler code-size increase. No dependency change.

## Test And Verification Plan

- Rebuild the MyLite storage-smoke MariaDB archive.
- Relink `mylite_embedded_storage_engine_test` and `mylite_perf_baseline`.
- Run focused storage-smoke coverage for routed storage and prepared statement
  reads.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run focused performance phases for prepared secondary exact selects,
  prepared published-leaf secondary exact selects, storage point lookups, and
  prepared primary-key selects.
- Run `git diff --check`.

## Acceptance Criteria

- Existing routed secondary-index SELECT coverage still passes.
- Repeated secondary cursor materialization no longer allocates a row-id array
  per cursor build.
- Local secondary-index performance improves or remains neutral.

## Risks And Unresolved Questions

- This is an incremental allocation reduction, not the full SQLite-like pager
  design. Advisory locks, checkpoint validation, SQL optimizer work, and row
  batch materialization remain larger read-path costs.
