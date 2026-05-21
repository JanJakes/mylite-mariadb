# Replication Helper Object Trim

## Problem

The default embedded profile now rejects replication and binary-log command
families, starts with binary logging disabled, omits SQL `BINLOG` replay, and
omits binary-log event read/write, GTID state, GTID index, replication filter,
replication sysvar, and row-replication conversion runtime. The archive still
carried four residual replication helper objects:

- `mariadb/sql/slave.cc`
- `mariadb/sql/sql_repl.cc`
- `mariadb/sql/rpl_utility.cc`
- `mariadb/sql/rpl_reporting.cc`

Those objects are replication server-topology code or row-event table-map
helpers. They are not ordinary SQL, native storage, JSON, GEOMETRY/GIS,
transactions, or public `libmylite` API behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The retained embedded archive no longer has undefined references to
  `init_*var_from_file()`, `master_info_index`,
  `Slave_reporting_capability`, `table_def`, or `event_checksum_test()`.
- `slave.cc.o` only contributed replication file-loading helpers and
  `master_info_index` in the current no-replication profile.
- `sql_repl.cc.o` contributed no code in the current embedded profile.
- `rpl_utility.cc.o` contributed table-map and event-checksum helpers that
  became unreferenced after event parser/server and row-conversion trims.
- `rpl_reporting.cc.o` contributed `Slave_reporting_capability`, now
  unreferenced because replication info objects are absent from the embedded
  archive.

## Design

Add `MYLITE_WITH_REPLICATION_HELPERS`, defaulting to `ON` for upstream-style
embedded builds and forced `OFF` in the MyLite embedded baseline.

When disabled:

- require the replication/binlog profile options that could reference these
  helpers to be disabled;
- remove `slave.cc`, `sql_repl.cc`, `rpl_utility.cc`, and `rpl_reporting.cc`
  from `SQL_EMBEDDED_SOURCES`;
- keep existing fail-closed binary-log, GTID, filter, and row-conversion stubs.

## Compatibility Impact

No supported SQL or public API behavior changes. Replication command families,
SQL `BINLOG`, GTID helper functions, GTID state assignments, replication
filters, and row-event apply are already outside the embedded core. Ordinary
SQL parsing, prepared statements, diagnostics, native storage engines,
transactions, JSON, GEOMETRY/GIS, sequence handling, and directory lifecycle
stay on retained non-replication paths.

## Directory And Lifecycle Impact

No file-format change and no new durable, temporary, lock, metadata, or runtime
paths. The slice only removes unreferenced replication helper objects from the
default embedded archive.

## Binary Size Impact

On this branch, omitting the residual replication helper objects reduced the
stripped archive from 26,180,192 bytes / 24.97 MiB to 26,170,360 bytes /
24.96 MiB. The archive member count dropped from 697 to 693. The pre-strip
archive moved from 26,742,160 bytes to 26,731,984 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
if ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg '(^|/)(slave|sql_repl|rpl_utility|rpl_reporting)\.cc\.o'; then
  exit 1
fi
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

- `MYLITE_WITH_REPLICATION_HELPERS=OFF` appears in the embedded CMake cache.
- `slave.cc.o`, `sql_repl.cc.o`, `rpl_utility.cc.o`, and
  `rpl_reporting.cc.o` are absent from `libmariadbd.a`.
- Retained fail-closed replication/binlog stubs remain present.
- Replication and binlog policy coverage still rejects unsupported server
  topology surfaces.
- Supported SQL, native storage, transactions, prepared statements, JSON,
  GEOMETRY/GIS, sequence handling, and directory lifecycle coverage still
  pass.
