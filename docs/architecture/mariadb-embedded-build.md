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
PLUGIN_AUTH_SOCKET=NO
PLUGIN_FEEDBACK=NO
PLUGIN_PERFSCHEMA=NO
PLUGIN_THREAD_POOL_INFO=NO
```

`WITH_WSREP=OFF` and `PLUGIN_S3=NO` are required because the initial MariaDB
import intentionally omits `wsrep-lib` and `storage/maria/libmarias3`.
Dynamic plugins, socket authentication, feedback, Performance Schema, and
thread-pool info are disabled because they are server-administration surfaces,
not core MyLite embedded runtime behavior.

On macOS, the profile also sets `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` to
`-Wno-nullability-completeness`. That keeps MariaDB's warning-as-error profile
compatible with recent macOS SDK headers without changing MyLite runtime
behavior.

## Measurement

Measured on 2026-05-15 from the imported MariaDB 11.8.6 source tree with the
MyLite embedded-restart patches applied.

| Field | Value |
| --- | --- |
| Host | macOS 26.4.1 25E253, arm64 |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.3.2 |
| Ninja | 1.13.2 |
| Bison | GNU Bison 3.8.2 from Homebrew |
| Archive | `build/mariadb-embedded/libmysqld/libmariadbd.a` |
| Archive size | 32,056,256 bytes / 30.57 MiB |
| Archive members | 692 |

The build found system OpenSSL 3.6.2, zlib, Curses, CURL, LibXml2, GSSAPI,
BZip2, LZ4, LibLZMA, LZO, PCRE2, and Zstandard support on this machine.

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
| Archive size | 32,197,960 bytes / 30.71 MiB |
| Archive members | 694 |

This smoke path now covers static plugin registration, current routed schema
namespaces and DDL/DML, sidecar gates, application-schema smoke, and
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
| MariaDB embedded archive | 32,056,256 bytes / 30.57 MiB | n/a | 692 | n/a |
| MariaDB storage-smoke archive | 32,197,960 bytes / 30.71 MiB | n/a | 694 | n/a |
| Embedded open-close smoke | 19,604,064 bytes / 18.70 MiB | 17,660,032 bytes / 16.84 MiB | n/a | 16,849 |
| Embedded exec smoke | 19,604,088 bytes / 18.70 MiB | 17,659,896 bytes / 16.84 MiB | n/a | 16,849 |
| Embedded statement smoke | 19,620,240 bytes / 18.71 MiB | 17,676,336 bytes / 16.86 MiB | n/a | 16,849 |
| Embedded warning smoke | 19,603,664 bytes / 18.70 MiB | 17,659,824 bytes / 16.84 MiB | n/a | 16,849 |
| Embedded comparison smoke | 19,700,688 bytes / 18.79 MiB | 17,710,512 bytes / 16.89 MiB | n/a | 16,851 |
| Storage-smoke open-close smoke | 19,677,328 bytes / 18.77 MiB | 17,710,272 bytes / 16.89 MiB | n/a | 16,849 |
| Storage-smoke exec smoke | 19,677,336 bytes / 18.77 MiB | 17,710,120 bytes / 16.89 MiB | n/a | 16,849 |
| Storage-smoke statement smoke | 19,677,024 bytes / 18.77 MiB | 17,710,064 bytes / 16.89 MiB | n/a | 16,849 |
| Storage-smoke warning smoke | 19,676,960 bytes / 18.77 MiB | 17,710,048 bytes / 16.89 MiB | n/a | 16,849 |
| Storage-smoke comparison smoke | 19,736,448 bytes / 18.82 MiB | 17,727,584 bytes / 16.91 MiB | n/a | 16,851 |
| Storage-engine smoke | 19,778,352 bytes / 18.86 MiB | 17,809,376 bytes / 16.98 MiB | n/a | 16,849 |

## Offline Build Caveat

This profile does not fetch MariaDB submodules, but upstream MariaDB's
`cmake/libfmt.cmake` downloads `fmt` 12.1.0 when a usable system `fmt` is not
configured. That download is a small third-party dependency fetch, not a MariaDB
source-tree expansion. A future CI/release slice should either configure a
system `fmt`, cache the external project, or vendor a reviewed dependency if
fully offline builds become a requirement.

## Follow-Up

Use this profile as the comparison point for later profile changes. Each future
trimming slice should record the size report, cache options, and compatibility
rationale.
