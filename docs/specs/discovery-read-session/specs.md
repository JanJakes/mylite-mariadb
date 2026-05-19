# Discovery Read Session

## Problem

After the checkpoint snapshot cache, prepared point-select profiling shows a new
large cost before cursor execution: MariaDB table open/discovery repeatedly
calls MyLite catalog discovery helpers. Those helpers read the table definition,
table list, or table existence through independent storage calls, so they still
open the primary file and validate header/catalog state outside the scoped
cursor read session.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_discover_table()` calls
  `mylite_storage_read_table_definition()` during table-share discovery.
- `mylite_discover_table_names()` and `mylite_discover_table_existence()` call
  `mylite_storage_list_tables()` and `mylite_storage_table_exists()`.
- The 2026-05-19 post-cache `sample` profile shows
  `read_statistics_for_tables()` / `open_system_tables_for_read()` /
  `ha_discover_table()` spending sampled time inside `mylite_discover_table()`,
  `mylite_storage_read_table_definition()`, `open_existing_file()`,
  `read_header()`, `read_catalog_root()`, and `checksum_page()`.

## Design

- Start a scoped MyLite storage read statement around each durable discovery
  callback:
  - table definition discovery;
  - discovered table-name enumeration;
  - table existence probing.
- Keep the scope local to the discovery callback. Do not hold a table-open read
  lock across MariaDB's broader statement lifetime.
- Let the storage read statement reuse the checkpoint snapshot cache when the
  durable header/catalog bytes are unchanged.

## Compatibility Impact

No SQL-visible behavior change is intended. The discovery callbacks read the
same catalog records as before, but through the same bounded read-session
lifetime used by cursor construction.

## Single-File And Lifecycle Impact

No file-format change and no new companion file. The scope is process-local and
ends before returning from the discovery callback.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

All durable routed table spellings benefit because MariaDB table discovery
resolves the MyLite catalog before handler execution. Runtime-volatile
`MEMORY` / `HEAP` rows still use volatile row storage after metadata discovery.

## Tests And Verification

- Existing storage-engine smoke coverage exercises discovery, table existence,
  reopened catalog metadata, routed DDL/DML, and sidecar gates.
- Run the storage-engine harness and the local performance baseline.

Local verification:

- `BUILD_DIR=build/mariadb-mylite-storage-smoke TARGET=mysqlserver tools/mariadb-embedded-build build`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 10000`
- `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`
- `git diff --check`

Performance evidence from the verified 1000-row, 10000-iteration local baseline:
direct primary-key point selects are `68.154 us/op`, prepared primary-key point
selects are `47.140 us/op`, direct published-leaf secondary exact selects are
`105.339 us/op`, and prepared published-leaf secondary exact selects are
`76.122 us/op`.

## Acceptance Criteria

- MyLite discovery callbacks use scoped storage read sessions.
- The storage-engine compatibility group remains green.
- The performance baseline shows reduced point-select overhead from table
  discovery/catalog validation.
