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
MYLITE_WITH_HELP_COMMAND=OFF
MYLITE_WITH_PROCEDURE_ANALYSE=OFF
MYLITE_WITH_SELECT_PROCEDURE_RUNTIME=OFF
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
MariaDB-specific `SFORMAT()` SQL function, SQL `HELP`,
`PROCEDURE ANALYSE()`, generic SELECT procedure runtime, socket
authentication, feedback, Performance Schema, and thread-pool info are disabled
because they are server-administration, blocking utility, Oracle
compatibility, legacy XML helper, spatial-function, MariaDB-specific
formatting, help-table lookup, result-set analysis, SELECT result-set extension
hook, or server/client file surfaces, not core MyLite embedded runtime
behavior. The retained `sql_embedded` C++ sources are also compiled with
`-fno-exceptions`; the flag is not applied to first-party MyLite code or to all
MariaDB targets.

On macOS, the profile also sets `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` to
`-Wno-nullability-completeness`. That keeps MariaDB's warning-as-error profile
compatible with recent macOS SDK headers without changing MyLite runtime
behavior.

## Measurement

Measured on 2026-05-15 from the imported MariaDB 11.8.6 source tree with the
current MyLite embedded profile patches applied.

| Field | Value |
| --- | --- |
| Host | macOS 26.4.1 25E253, arm64 |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.3.2 |
| Ninja | 1.13.2 |
| Bison | GNU Bison 3.8.2 from Homebrew |
| Archive | `build/mariadb-embedded/libmysqld/libmariadbd.a` |
| Archive size | 28,117,968 bytes / 26.82 MiB |
| Archive members | 688 |

The build found system OpenSSL 3.6.2, zlib, Curses, CURL, GSSAPI, BZip2, LZ4,
LibLZMA, LZO, PCRE2, and Zstandard support on this machine.

The SELECT procedure runtime trim reduced the default archive by 42,088 bytes
from the previous `PROCEDURE ANALYSE()` baseline and removed one archive
member.

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
- SQL `HELP` command help-table implementation
- `PROCEDURE ANALYSE()` result-set analysis implementation
- generic SELECT procedure runtime
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

Measured on 2026-05-15 with the same host and toolchain as the default profile:

| Field | Value |
| --- | --- |
| Archive | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` |
| Archive size | 28,298,552 bytes / 26.99 MiB |
| Archive members | 691 |

This is 42,088 bytes smaller than the previous `PROCEDURE ANALYSE()`
storage-smoke archive and removes one archive member.

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

Measured on 2026-05-15 with the current embedded and storage-smoke build
outputs:

| Artifact | Size | Stripped Size | Members | Global Symbols |
| --- | ---: | ---: | ---: | ---: |
| MariaDB embedded archive | 28,117,968 bytes / 26.82 MiB | n/a | 688 | n/a |
| MariaDB storage-smoke archive | 28,298,552 bytes / 26.99 MiB | n/a | 691 | n/a |
| Embedded open-close smoke | 17,936,496 bytes / 17.11 MiB | 16,234,976 bytes / 15.48 MiB | n/a | 16,163 |
| Embedded exec smoke | 17,954,376 bytes / 17.12 MiB | 16,251,384 bytes / 15.50 MiB | n/a | 16,163 |
| Embedded statement smoke | 17,952,688 bytes / 17.12 MiB | 16,251,264 bytes / 15.50 MiB | n/a | 16,163 |
| Embedded warning smoke | 17,936,096 bytes / 17.11 MiB | 16,234,752 bytes / 15.48 MiB | n/a | 16,163 |
| Embedded comparison smoke | 18,042,816 bytes / 17.21 MiB | 16,285,584 bytes / 15.53 MiB | n/a | 16,165 |
| Storage-smoke open-close smoke | 18,015,104 bytes / 17.18 MiB | 16,285,312 bytes / 15.53 MiB | n/a | 16,163 |
| Storage-smoke exec smoke | 18,049,512 bytes / 17.21 MiB | 16,318,248 bytes / 15.56 MiB | n/a | 16,163 |
| Storage-smoke statement smoke | 18,047,856 bytes / 17.21 MiB | 16,318,096 bytes / 15.56 MiB | n/a | 16,163 |
| Storage-smoke warning smoke | 18,014,704 bytes / 17.18 MiB | 16,285,072 bytes / 15.53 MiB | n/a | 16,163 |
| Storage-smoke comparison smoke | 18,116,944 bytes / 17.28 MiB | 16,335,760 bytes / 15.58 MiB | n/a | 16,165 |
| Storage-engine smoke | 18,283,792 bytes / 17.44 MiB | 16,549,760 bytes / 15.78 MiB | n/a | 16,163 |

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
