# MariaDB Embedded Build

This document records the first reproducible MariaDB embedded-library baseline
for MyLite. The baseline is intentionally broad: it proves the imported MariaDB
source can build an embedded archive before MyLite starts trimming server
surface or adding storage patches.

## Command

```sh
tools/mariadb-embedded-build all
```

The wrapper configures MariaDB with
`cmake/mariadb-embedded-baseline.cmake`, builds only the `libmariadbd.a` target,
and reports archive size evidence. It does not update MariaDB submodules.

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
```

`WITH_WSREP=OFF` and `PLUGIN_S3=NO` are required because the initial MariaDB
import intentionally omits `wsrep-lib` and `storage/maria/libmarias3`.

## Measurement

Measured on 2026-05-14 from the imported MariaDB 11.8.6 source tree with the
MyLite embedded-restart patches applied.

| Field | Value |
| --- | --- |
| Host | macOS 26.4.1 25E253, arm64 |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.3.2 |
| Ninja | 1.13.2 |
| Bison | GNU Bison 3.8.2 from Homebrew |
| Archive | `build/mariadb-embedded/libmysqld/libmariadbd.a` |
| Archive size | 33,736,928 bytes / 32.17 MiB |
| Archive members | 807 |

The build found system OpenSSL 3.6.2, zlib, Curses, CURL, LibXml2, GSSAPI,
BZip2, LZ4, LibLZMA, LZO, PCRE2, and Zstandard support on this machine.

## Enabled Surface

The profile leaves most MariaDB defaults intact. The embedded archive includes
the static embedded server library and static embedded engines/plugins such as:

- Aria
- CSV
- HEAP/MEMORY
- InnoDB
- MyISAM and MRG_MyISAM
- Performance Schema
- Sequence and partition support
- selected static server plugins such as auth socket, feedback, type handlers,
  user variables, userstat, and thread-pool info

Configure also enables many module targets, including Archive, Blackhole,
CONNECT, Example, Federated, FederatedX, Mroonga, Sphinx, Spider, and many
server plugins. The `libmariadbd.a` target does not build every configured
module, but the enabled list is still important size-profile evidence because
future profile hardening should disable unwanted surfaces intentionally.

## Disabled Or Missing Surface

The baseline explicitly disables:

- WSREP/Galera
- Aria S3 support
- MariaDB upstream unit-test targets

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

Measured on 2026-05-14 with the same host and toolchain as the baseline:

| Field | Value |
| --- | --- |
| Archive | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` |
| Archive size | 33,778,328 bytes / 32.21 MiB |
| Archive members | 808 |

This smoke path now covers static plugin registration, current routed
DDL/DML, sidecar gates, application-schema smoke, and representative
server-surface policy. It is still opt-in so the default embedded baseline
remains separate from the MyLite handler build.

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
| MariaDB embedded archive | 33,736,928 bytes / 32.17 MiB | n/a | 807 | n/a |
| MariaDB storage-smoke archive | 33,851,288 bytes / 32.28 MiB | n/a | 809 | n/a |
| Embedded open-close smoke | 20,297,088 bytes / 19.36 MiB | 18,201,680 bytes / 17.36 MiB | n/a | 17,004 |
| Embedded exec smoke | 20,296,872 bytes / 19.36 MiB | 18,201,496 bytes / 17.36 MiB | n/a | 17,004 |
| Storage-smoke open-close smoke | 20,321,536 bytes / 19.38 MiB | 18,218,704 bytes / 17.37 MiB | n/a | 17,004 |
| Storage-smoke exec smoke | 20,321,304 bytes / 19.38 MiB | 18,218,536 bytes / 17.37 MiB | n/a | 17,004 |
| Storage-engine smoke | 20,371,808 bytes / 19.43 MiB | 18,268,080 bytes / 17.42 MiB | n/a | 17,004 |

## Offline Build Caveat

This profile does not fetch MariaDB submodules, but upstream MariaDB's
`cmake/libfmt.cmake` downloads `fmt` 12.1.0 when a usable system `fmt` is not
configured. That download is a small third-party dependency fetch, not a MariaDB
source-tree expansion. A future CI/release slice should either configure a
system `fmt`, cache the external project, or vendor a reviewed dependency if
fully offline builds become a requirement.

## Follow-Up

Use this baseline as the comparison point for later profile changes. Each
future trimming slice should record the size report, cache options, and
compatibility rationale.
