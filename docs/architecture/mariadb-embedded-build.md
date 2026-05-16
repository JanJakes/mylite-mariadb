# MariaDB Embedded Build

This document records the reproducible MariaDB embedded-library profile for
MyLite. The profile starts from MariaDB's embedded server build and applies
measured trims for server-oriented surfaces outside the file-owned embedded
runtime.

## Command

```sh
tools/mariadb-embedded-build all
```

The wrapper configures MariaDB with
`cmake/mariadb-embedded-baseline.cmake`, builds only the `libmariadbd.a` target,
and reports archive size evidence. It does not update MariaDB submodules.
On macOS, it prefers `/Library/Developer/CommandLineTools` when that developer
directory is installed and `DEVELOPER_DIR` is not already set, keeping the C and
C++ SDK headers from the same toolchain.

## Profile

The committed baseline cache sets:

```text
CMAKE_BUILD_TYPE=Release
UPDATE_SUBMODULES=OFF
WITH_EMBEDDED_SERVER=ON
WITH_SSL=system
WITH_UNIT_TESTS=OFF
WITH_WSREP=OFF
PLUGIN_S3=NO
WITHOUT_DYNAMIC_PLUGINS=ON
MYLITE_WITH_LOAD_DATA=OFF
MYLITE_WITH_SQL_FILE_IO=OFF
MYLITE_WITH_SERVER_UTILITY_FUNCTIONS=OFF
MYLITE_WITH_ORACLE_SQL_MODE=OFF
MYLITE_WITH_XML_SQL_FUNCTIONS=OFF
MYLITE_WITH_GIS_SQL_FUNCTIONS=OFF
MYLITE_WITH_SFORMAT_SQL_FUNCTION=OFF
MYLITE_WITH_JSON_SCHEMA_VALID=OFF
MYLITE_WITH_JSON_TABLE=OFF
MYLITE_WITH_HELP_COMMAND=OFF
MYLITE_WITH_PROCEDURE_ANALYSE=OFF
MYLITE_WITH_SELECT_PROCEDURE_RUNTIME=OFF
MYLITE_WITH_STORED_PROGRAM_RUNTIME=OFF
MYLITE_WITH_UDF_RUNTIME=OFF
MYLITE_WITH_BINLOG_CORE=OFF
MYLITE_WITH_MYISAM_MAINTENANCE=OFF
MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS=OFF
PLUGIN_AUTH_SOCKET=NO
PLUGIN_FEEDBACK=NO
PLUGIN_PERFSCHEMA=NO
PLUGIN_THREAD_POOL_INFO=NO
```

`WITH_WSREP=OFF` and `PLUGIN_S3=NO` are required because the initial MariaDB
import intentionally omits `wsrep-lib` and `storage/maria/libmarias3`.
Dynamic plugins, LOAD file import, SQL host-file I/O, server utility SQL
functions, Oracle SQL mode parsing, XML SQL functions, GIS SQL functions, the
MariaDB-specific `SFORMAT()` SQL function, JSON schema validation, the
`JSON_TABLE` table-function runtime, SQL `HELP`, `PROCEDURE ANALYSE()`, generic
SELECT procedure runtime, stored-program runtime, dynamic UDF lookup/execution,
binary-log transaction, event-write, and event-root core, native MyISAM
table-maintenance and key-cache administration, socket authentication,
feedback, Performance Schema, and thread-pool info are
disabled
because they are server-administration, blocking utility, Oracle
compatibility, legacy XML helper, spatial-function, MariaDB-specific
formatting, schema validation, table-function projection, help-table lookup,
result-set analysis, SELECT result-set extension hook, unsupported non-table
object, dynamic extension, server topology, engine-file maintenance, or
server/client file surfaces, not core MyLite embedded runtime behavior. The
retained `sql_embedded` C++ sources are also compiled with `-fno-exceptions`;
the flag is not applied to first-party MyLite code or to all MariaDB targets.

On macOS, the profile also sets `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` to
`-Wno-nullability-completeness`. That keeps MariaDB's warning-as-error profile
compatible with recent macOS SDK headers without changing MyLite runtime
behavior.

## Measurement

Measured on 2026-05-16 from the imported MariaDB 11.8.6 source tree with the
current MyLite embedded profile patches applied.

| Field | Value |
| --- | --- |
| Host | macOS 26.4.1 25E253, arm64 |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.3.2 |
| Ninja | 1.13.2 |
| Bison | GNU Bison 3.8.2 from Homebrew |
| Archive | `build/mariadb-embedded/libmysqld/libmariadbd.a` |
| Archive size | 27,370,344 bytes / 26.10 MiB |
| Archive members | 675 |

The build found system OpenSSL 3.6.2, zlib, Curses, CURL, GSSAPI, BZip2, LZ4,
LibLZMA, LZO, PCRE2, and Zstandard support on this machine.

The binlog event-root trim reduced the default archive by 146,128 bytes from
the previous no-binlog transaction/event-core baseline and removed two archive
members. The disabled profile now omits `gtid_index.cc`, `log_event.cc`,
`rpl_injector.cc`, and `rpl_record.cc`, skips the mandatory binlog plugin
registration, and compiles embedded binlog transaction, row-event, GTID-state,
event-write, table-map, open/recovery, GTID-index, incident, cache-write, and
temporary-table binlog entry points to no-ops. `log_event_server.cc` and
`rpl_gtid.cc` remain in the archive because retained MariaDB code still
references shared helpers and GTID state, but linked first-party embedded test
binaries resolve the ordinary SQL string-rendering root from
`mylite_log_event_core_stub.cc` instead of loading `log_event_server.cc.o`.

The MyISAM maintenance trim reduced the default archive by a further 87,712
bytes and removed three archive members. The disabled profile now omits
`mi_check.c`, `mi_keycache.c`, and `mi_preload.c`, rejects direct and prepared
table-maintenance and key-cache administration SQL before MariaDB execution,
returns unsupported handler status for native MyISAM maintenance entry points,
and leaves ordinary routed `ENGINE=MyISAM` DDL/DML covered through MyLite
storage.

The JSON schema validation trim reduced the default archive by a further
104,760 bytes and removed one archive member. The disabled profile now omits
`json_schema.cc`, rejects direct and prepared `JSON_SCHEMA_VALID()` calls before
MariaDB execution, and retains `json_schema_helper.cc` because ordinary JSON
functions use its hash-key helper.

The JSON table-function trim reduced the default archive by a further 44,400
bytes with the same member count. The disabled profile now replaces
`json_table.cc` with a MyLite disabled stub, rejects direct and prepared
`JSON_TABLE(...)` calls before MariaDB execution, and leaves ordinary JSON
scalar/path helpers available.

## Enabled Surface

The profile keeps the MariaDB components needed by the current embedded
bootstrap and storage-routing smoke coverage. The embedded archive includes the
static embedded server library and static embedded engines/plugins such as:

- Aria
- CSV
- HEAP/MEMORY
- InnoDB
- MyISAM and MRG_MyISAM
- Sequence and partition support
- selected compatibility helpers such as type handlers, user variables, and
  userstat

Some configured module metadata remains visible even when the
`libmariadbd.a` target does not build the module into the embedded archive.
Keep treating the configured surface as size-profile evidence: future profile
hardening should disable unwanted surfaces intentionally and record the effect.

## Disabled Or Missing Surface

The profile explicitly disables:

- WSREP/Galera
- Aria S3 support
- MariaDB upstream unit-test targets
- dynamic plugin support
- LOAD DATA / LOAD XML execution support
- host-file SQL I/O support for `LOAD_FILE()` and `SELECT ... INTO OUTFILE` /
  `DUMPFILE`
- server utility SQL functions including `BENCHMARK()`, named-lock helpers,
  replication wait helpers, `SLEEP()`, and `UUID_SHORT()`
- Oracle SQL mode parser
- XML SQL functions `EXTRACTVALUE()` and `UPDATEXML()`
- GIS SQL functions including `ST_AsText()`, `ST_GeomFromText()`,
  `ST_Contains()`, `PointFromText()`, `Point()`, and `X()`
- MariaDB-specific `SFORMAT()` SQL function
- `JSON_SCHEMA_VALID()` SQL function and schema-validator keyword runtime
- `JSON_TABLE()` table-function runtime
- SQL `HELP` command help-table implementation
- `PROCEDURE ANALYSE()` result-set analysis implementation
- generic SELECT procedure runtime
- stored routine, trigger, event, package, and stored-program instruction
  runtime
- dynamic UDF lookup, registration, and execution runtime
- binary-log transaction, event-write, and event-root core for the embedded
  no-binlog profile
- native MyISAM table maintenance, repair, key-cache assignment, and key
  preload administration
- C++ exception support in retained `sql_embedded` C++ compilation
- socket authentication
- feedback plugin
- Performance Schema
- thread-pool info plugin

After the storage-engine skeleton slice, MariaDB configure also discovers
`MYLITE_SE` and leaves it disabled by default. Opt-in handler smoke builds use
`-DPLUGIN_MYLITE_SE=STATIC`; that path is not part of this baseline size.

Configure also reports unavailable optional features on this host, including
RocksDB, OQGraph, AWS key management, CONNECT JDBC, and Snappy.

## Opt-In Storage Handler Smoke

The MyLite storage-engine handler registration smoke uses a separate MariaDB
build directory so the baseline archive above stays unchanged:

```sh
BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all \
  -DPLUGIN_MYLITE_SE=STATIC
cmake --preset storage-smoke-dev
cmake --build --preset storage-smoke-dev
ctest --preset storage-smoke-dev
```

Measured on 2026-05-16 with the same host and toolchain as the default profile:

| Field | Value |
| --- | --- |
| Archive | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` |
| Archive size | 27,550,928 bytes / 26.27 MiB |
| Archive members | 678 |

This is 44,400 bytes smaller than the previous JSON-schema-validation-trim
storage-smoke archive with the same archive member count.

This smoke path now covers static plugin registration, current routed schema
namespaces and DDL/DML, BLACKHOLE row-discard routing, MEMORY/HEAP volatile-row
routing, sidecar gates, application-schema smoke, and
representative server-surface policy. It is still opt-in so the default
embedded profile remains separate from the MyLite handler build.

## Size Report

Use the first-party size report when evaluating profile hardening:

```sh
tools/mylite-size-report
```

The report keeps archive measurements beside linked MyLite runtime proxies,
because source and linker changes can affect `libmariadbd.a` and the final
linked payload differently.

Measured on 2026-05-16 with the current embedded and storage-smoke build
outputs:

| Artifact | Size | Stripped Size | Members | Global Symbols |
| --- | ---: | ---: | ---: | ---: |
| MariaDB embedded archive | 27,370,344 bytes / 26.10 MiB | n/a | 675 | n/a |
| MariaDB storage-smoke archive | 27,550,928 bytes / 26.27 MiB | n/a | 678 | n/a |
| Embedded open-close smoke | 17,438,288 bytes / 16.63 MiB | 15,772,912 bytes / 15.04 MiB | n/a | 15,408 |
| Embedded exec smoke | 17,473,064 bytes / 16.66 MiB | 15,805,880 bytes / 15.07 MiB | n/a | 15,408 |
| Embedded statement smoke | 17,471,104 bytes / 16.66 MiB | 15,805,744 bytes / 15.07 MiB | n/a | 15,408 |
| Embedded warning smoke | 17,454,448 bytes / 16.65 MiB | 15,789,232 bytes / 15.06 MiB | n/a | 15,408 |
| Embedded comparison smoke | 17,561,184 bytes / 16.75 MiB | 15,840,048 bytes / 15.11 MiB | n/a | 15,410 |
| Storage-smoke open-close smoke | 17,533,424 bytes / 16.72 MiB | 15,839,776 bytes / 15.11 MiB | n/a | 15,408 |
| Storage-smoke exec smoke | 17,568,168 bytes / 16.75 MiB | 15,872,712 bytes / 15.14 MiB | n/a | 15,408 |
| Storage-smoke statement smoke | 17,566,240 bytes / 16.75 MiB | 15,872,592 bytes / 15.14 MiB | n/a | 15,408 |
| Storage-smoke warning smoke | 17,533,056 bytes / 16.72 MiB | 15,839,568 bytes / 15.11 MiB | n/a | 15,408 |
| Storage-smoke comparison smoke | 17,635,296 bytes / 16.82 MiB | 15,890,240 bytes / 15.15 MiB | n/a | 15,410 |
| Storage-engine smoke | 17,802,112 bytes / 16.98 MiB | 16,104,224 bytes / 15.36 MiB | n/a | 15,408 |

## Offline Build Caveat

This profile does not fetch MariaDB submodules. Upstream MariaDB's
`cmake/libfmt.cmake` still defines bundled `fmt` 12.1.0 external-project rules
when a usable system `fmt` is not configured, but the default MyLite embedded
and storage-smoke archive targets no longer depend on that target when
`SFORMAT()` is disabled. A future CI/release slice should still either
configure a system `fmt`, cache the external project, or vendor a reviewed
dependency if fully offline builds become a requirement for broader target
sets.

## Follow-Up

Use this profile as the comparison point for later profile changes. Each future
trimming slice should record the size report, cache options, and compatibility
rationale.
