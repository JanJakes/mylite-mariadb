# Native CSV Engine Trim

## Problem

The default embedded profile still registers MariaDB's native `CSV` storage
engine and merges `ha_tina.cc.o` plus `transparent_file.cc.o` into
`libmariadbd.a`. CSV stores table contents in external `.CSV` and `.CSM` files,
which does not fit MyLite's default single-file runtime.

Unlike the previous native engine trims, upstream marks CSV as a mandatory
plugin. Passing `PLUGIN_CSV=NO` is ignored by MariaDB's plugin CMake machinery,
so this slice needs a narrow MyLite-owned upstream-derived opt-out instead of
only a baseline cache change.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/csv/CMakeLists.txt` registers CSV as
  `MYSQL_ADD_PLUGIN(csv ${CSV_SOURCES} STORAGE_ENGINE MANDATORY)`.
- `mariadb/cmake/plugin.cmake` forces mandatory plugins to `PLUGIN_<NAME>=YES`
  and unsets any caller-provided cache value before plugin selection. This is
  why a previous `PLUGIN_CSV=NO` probe left CSV enabled.
- `mariadb/storage/csv/ha_tina.cc` declares the `CSV` plugin, sets
  `DB_TYPE_CSV_DB`, advertises `HTON_SUPPORT_LOG_TABLES`, and opens/writes
  external CSV metadata/data/update files through the native handler.
- `mariadb/sql/mysqld.cc` initializes storage engines before CSV logging and
  falls back from `LOG_TABLE` to `LOG_FILE` when the `csv` plugin is not ready.
- `mariadb/scripts/mariadb_system_tables.sql` creates `mysql.general_log` and
  `mysql.slow_log` only when `information_schema.engines` reports `CSV`
  support, so bootstrap can tolerate an absent CSV engine.

## Design

- Add a MyLite-specific cache option, `MYLITE_WITH_CSV_STORAGE_ENGINE`, default
  `ON` in the upstream-derived CSV CMake file to preserve MariaDB's default
  source behavior outside MyLite profiles.
- Set `MYLITE_WITH_CSV_STORAGE_ENGINE=OFF` in the MyLite embedded baseline.
- When the option is `OFF`, call `MYSQL_ADD_PLUGIN(csv ... STORAGE_ENGINE
  DISABLED)` instead of the upstream mandatory registration. This keeps the
  plugin-cache metadata explicit as `PLUGIN_CSV=NO` without compiling the CSV
  handler objects or registering `CSV` as mandatory.
- Keep MyLite's existing file-import/export rejection policy unchanged. This
  slice removes native CSV table storage, not SQL file I/O parsing.
- Extend server-surface coverage, MTR cache validation, size tooling, and docs
  so the disabled profile proves that native CSV is absent.
- Update the MTR bootstrap-schema expectation because `mysql.general_log` and
  `mysql.slow_log` are not created when CSV is absent.

## Affected Subsystems

- MariaDB CSV storage-engine CMake registration.
- MariaDB embedded baseline profile.
- MTR smoke profile validation and bootstrap-schema expectation.
- Server-surface tests and compatibility documentation.
- Size-profile measurement documentation.

## MySQL/MariaDB Compatibility Impact

Native MariaDB `ENGINE=CSV` tables are unsupported in the default MyLite
embedded profile. This is intentional: CSV is an external-file storage engine
and does not preserve MyLite's single-primary-file product contract.

This does not change MyLite's supported routed engine set. Ordinary routed
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted/default-engine,
`ENGINE=BLACKHOLE`, and `ENGINE=MEMORY` / `ENGINE=HEAP` behavior remains the
compatibility target.

## DDL Metadata Routing Impact

No catalog format change. MyLite does not claim CSV table compatibility in the
current routing policy.

## Single-File And Embedded-Lifecycle Impact

The trim removes native CSV handler objects that can create external `.CSV`,
`.CSM`, and update files. MariaDB bootstrap should continue because system log
tables are conditional on CSV support and server startup falls back to file
logging when table logging requests cannot use CSV.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

Native `CSV` no longer appears in `SHOW ENGINES`. Applications requiring CSV
table files remain unsupported until MyLite either maps that syntax to a
single-file storage contract or explicitly rejects it with a MyLite-owned
diagnostic.

## Binary-Size Impact

The implemented default embedded profile measures 20,638,344 bytes / 19.68
MiB with 478 archive members, a reduction of 61,440 bytes and two members from
the previous no-partition baseline.

The implemented storage-smoke profile measures 20,833,736 bytes / 19.87 MiB
with 481 archive members, a reduction of 61,448 bytes and two members from the
previous storage-smoke baseline.

## License And Dependency Impact

No new dependency. The change removes MariaDB-derived native CSV handler
objects from the disabled embedded profile.

## Test And Verification Plan

- Configure, build, and measure the default embedded archive.
- Configure, build, and measure the storage-smoke embedded archive with
  `PLUGIN_MYLITE_SE=STATIC`.
- Confirm default and storage-smoke archives omit `ha_tina.cc.o` and
  `transparent_file.cc.o`.
- Confirm `SHOW ENGINES` does not advertise `CSV`.
- Confirm existing routed engine coverage still passes.
- Confirm MTR smoke bootstrap and scalar CAST/CONVERT tests pass with CSV
  absent, and update bootstrap-schema expected results for absent log tables.
- Run relevant CMake presets, CTest presets, compatibility harness groups, MTR
  smoke, format/static checks, shell checks, diff hygiene, and size report.

## Implementation Verification

Completed on 2026-05-16:

- `tools/mariadb-embedded-build all`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- Archive member probes for `ha_tina.cc.o` and `transparent_file.cc.o`
- `cmake --preset dev`, `cmake --preset embedded-dev`, and
  `cmake --preset storage-smoke-dev`
- `cmake --build --preset dev`, `embedded-dev`, and `storage-smoke-dev`
- `ctest --preset dev --output-on-failure`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-mtr-harness run`
- `tools/mylite-compat-harness report server-surface storage-engine sidecar partition`
- `tools/mylite-size-report`

## Acceptance Criteria

- The embedded baseline sets `MYLITE_WITH_CSV_STORAGE_ENGINE=OFF`.
- Default, storage-smoke, and MTR smoke caches record `PLUGIN_CSV=NO`.
- Default and storage-smoke archives omit `ha_tina.cc.o` and
  `transparent_file.cc.o`.
- `SHOW ENGINES` omits native `CSV`.
- MTR bootstrap succeeds without `mysql.general_log` and `mysql.slow_log`.
- Size docs record the measured reduction.

## Risks And Open Questions

- Some upstream MTR suites assume CSV and source `include/have_csv.inc`; the
  current MyLite MTR smoke subset must stay curated until broader comparison
  infrastructure can select CSV-dependent tests conditionally.
- Explicit `ENGINE=CSV` application DDL is handled by the separate CSV engine
  request policy, which rejects it with a MyLite-owned diagnostic before
  catalog publication.
