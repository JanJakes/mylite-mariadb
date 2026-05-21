# Binlog GTID Index Trim

## Problem

The default embedded profile rejects replication and binary-log command
families, starts with binary logging disabled, omits SQL `BINLOG` replay, and
omits binary-log event read/write runtime. The archive still carried
MariaDB's binary-log GTID index implementation from `mariadb/sql/gtid_index.cc`
plus GTID-index tuning variables.

The GTID index is an on-disk index for binary-log files. It speeds up binlog
offset and GTID-position lookup for replication clients and
`BINLOG_GTID_POS()`. That is server topology behavior, not ordinary SQL,
native storage, JSON, GEOMETRY/GIS, transactions, or public `libmylite` API
behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/gtid_index.h` documents the GTID index as an on-disk index for
  each binary-log file, primarily used when a replica connects to a primary and
  by `BINLOG_GTID_POS()`.
- The default embedded profile already starts with `--skip-log-bin`, reports
  `@@log_bin=0`, rejects replication and binlog SQL, omits
  `rpl_injector.cc`, `rpl_record.cc`, `sql_binlog.cc`, `log_event.cc`,
  `log_event_server.cc`, and `rpl_gtid.cc`, and guards no-binlog GTID-index
  update paths.
- Retained MariaDB no-binlog paths still need GTID-index class symbols to
  link, but they must not create or read binary-log index files.
- `binlog_gtid_index`, `binlog_gtid_index_page_size`, and
  `binlog_gtid_index_span_min` tune binary-log GTID indexing and have no
  supported meaning in the embedded no-binlog profile.

## Design

Add `MYLITE_WITH_GTID_INDEX`, defaulting to `ON` for upstream-style embedded
builds and forced `OFF` in the MyLite embedded baseline.

When disabled:

- omit `gtid_index.cc` from `libmysqld` embedded SQL sources;
- reject the invalid custom-profile combination where GTID indexing is disabled
  while the active binary-log core is enabled;
- link `mylite_gtid_index_disabled.cc` as a minimal fail-closed GTID-index
  class contract;
- return no results for GTID-index searches and fail GTID-index writes;
- omit `binlog_gtid_index`, `binlog_gtid_index_page_size`, and
  `binlog_gtid_index_span_min` system-variable rows from the embedded profile.

## Compatibility Impact

No supported SQL or public API behavior changes. Binary-log GTID indexing,
replication position lookup, and binlog GTID-index tuning are already outside
the embedded core. `@@log_bin=0` remains covered. Ordinary SQL parsing,
prepared statements, diagnostics, native storage engines, transactions, JSON,
GEOMETRY/GIS, sequence handling, and directory lifecycle stay on retained
non-binary-log paths.

## Directory And Lifecycle Impact

No file-format change and no new durable, temporary, lock, metadata, or runtime
paths. The slice removes binary-log GTID-index implementation from the default
embedded archive and keeps unsupported GTID-index paths fail-closed, so no
`.idx` binary-log index files are created.

## Binary Size Impact

On this branch, omitting `gtid_index.cc.o` and linking the disabled embedded
source reduced the stripped archive from 26,195,576 bytes / 24.98 MiB to
26,180,192 bytes / 24.97 MiB. The archive member count stayed at 697. The
pre-strip archive moved from 26,758,104 bytes to 26,742,160 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg 'gtid_index|mylite_gtid_index'
tools/mariadb-embedded-build measure
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_GTID_INDEX=OFF` appears in the embedded CMake cache.
- `gtid_index.cc.o` is absent from `libmariadbd.a`.
- `mylite_gtid_index_disabled.cc.o` is present in `libmariadbd.a`.
- `binlog_gtid_index`, `binlog_gtid_index_page_size`, and
  `binlog_gtid_index_span_min` are absent from the embedded system-variable
  surface.
- Replication and binlog policy coverage still rejects unsupported server
  topology surfaces.
- Supported SQL, native storage, transactions, prepared statements, JSON,
  GEOMETRY/GIS, sequence handling, and directory lifecycle coverage still
  pass.
