# MariaDB Embedded Build

This document records the reproducible MariaDB embedded-library baseline for
MyLite. The baseline keeps core SQL, native storage, and application-facing
types intact while trimming server-only surfaces that do not fit the embedded
runtime.

## Command

```sh
tools/mariadb-embedded-build all
```

The wrapper configures MariaDB with
`cmake/mariadb-embedded-baseline.cmake`, builds only the `libmariadbd.a` target,
strips debug and local-symbol metadata from the archive, and reports archive
size evidence. It does not update MariaDB submodules.

Set `STRIP_ARCHIVE=0` when an unstripped archive is needed for local
inspection.

## Profile

The committed baseline cache sets:

```text
CMAKE_BUILD_TYPE=MinSizeRel
UPDATE_SUBMODULES=OFF
WITH_EMBEDDED_SERVER=ON
WITH_SSL=system
WITH_ZLIB=bundled
WITH_UNIT_TESTS=OFF
WITH_WSREP=OFF
PLUGIN_S3=NO
PLUGIN_PERFSCHEMA=NO
PLUGIN_FEEDBACK=NO
ENABLED_PROFILING=OFF
MYLITE_WITH_BINLOG_CORE=OFF
MYLITE_WITH_PROCEDURE_ANALYSE=OFF
MYLITE_WITH_SYSVAR_HELP_TEXT=OFF
MYLITE_WITH_STATIC_SHOW_INFO=OFF
```

`CMAKE_BUILD_TYPE=MinSizeRel` makes the embedded MariaDB archive optimize for
size without changing compiled runtime surface. `WITH_ZLIB=bundled` prevents
the system zlib lookup from adding the macOS SDK root include directory as a
normal `-I` path. The embedded archive includes the bundled zlib objects, so
`libmylite` does not add a separate host zlib link.
`WITH_WSREP=OFF` and
`PLUGIN_S3=NO` are required because the initial MariaDB import intentionally
omits `wsrep-lib` and `storage/maria/libmarias3`.
`PLUGIN_PERFSCHEMA=NO` removes the unused Performance Schema static plugin
from the embedded archive. If a custom MariaDB build includes Performance
Schema, MyLite still passes `--performance-schema=OFF`; otherwise the omitted
plugin is the disabled contract. `PLUGIN_FEEDBACK=NO` omits MariaDB's telemetry
and usage-reporting plugin from the embedded profile. `ENABLED_PROFILING=OFF`
omits statement-profiling implementation code while preserving MariaDB's
disabled `@@have_profiling=NO` contract. The embedded query cache is compiled
to no-op stubs and reports `@@have_query_cache=NO`. The embedded archive links
a small Oracle SQL-mode parser stub instead of the generated Oracle parser. It
also omits the fmtlib-backed `SFORMAT()` SQL function and builds the embedded
SQL target without C++ exceptions or unwind tables. Dynamic UDF shared-library
loading is omitted from the embedded archive. The embedded baseline disables
the active binary-log transaction/event core behind `MYLITE_WITH_BINLOG_CORE=0`
while preserving the normal MariaDB server build path. It also omits the
legacy `PROCEDURE ANALYSE()` implementation behind
`MYLITE_WITH_PROCEDURE_ANALYSE=0`. Long system-variable help comments are
omitted behind `MYLITE_WITH_SYSVAR_HELP_TEXT=0`; variable names, values,
defaults, validation, and `SHOW VARIABLES` remain intact. Static `SHOW
AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` result producers are
omitted behind `MYLITE_WITH_STATIC_SHOW_INFO=0`.

## Measurement

Measured on 2026-05-20 from the imported MariaDB 11.8.6 source tree with the
MyLite embedded-restart patches applied and post-build archive stripping
enabled.

| Field | Value |
| --- | --- |
| Host | macOS 26.4.1 25E253, arm64 |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.3.2 |
| Ninja | 1.13.2 |
| Bison | GNU Bison 3.8.2 from Homebrew |
| Archive | `build/mariadb-embedded/libmysqld/libmariadbd.a` |
| Archive size | 27,137,632 bytes / 25.88 MiB |
| Archive members | 705 |

The original broad archive before safe size hardening was 33,842,320 bytes /
32.27 MiB. With `MinSizeRel`, the unused Performance Schema static plugin
disabled, the Feedback plugin omitted, statement profiling disabled, and
embedded `HELP` compiled to an unsupported-command stub, the embedded query
cache stubbed, the Oracle SQL-mode parser replaced by an unsupported stub, and
embedded `SFORMAT()` omitted so the embedded SQL target can compile without C++
exceptions, and unwind tables omitted from that exception-free target, the
pre-strip archive is 28,026,280 bytes / 26.73 MiB. Omitting dynamic UDF
runtime reduces the pre-strip archive to 27,938,032 bytes / 26.64 MiB.
Omitting the embedded binary-log core reduces the pre-strip archive to
27,864,688 bytes / 26.57 MiB. Omitting `PROCEDURE ANALYSE()` reduces the
pre-strip archive to 27,825,136 bytes / 26.54 MiB. Omitting system-variable
help text reduces the pre-strip archive to 27,767,568 bytes / 26.48 MiB.
Omitting static `SHOW` information reduces the pre-strip archive to 27,732,624
bytes / 26.45 MiB. Post-build `strip -S -x` plus `ranlib` saves another
594,992 bytes without changing archive membership or runtime behavior. The
`SFORMAT()` and exception cut accounts for 1,808,240
bytes, unwind-table omission saves another 10,840 bytes, and dynamic UDF
runtime omission saves 87,416 bytes and one archive member. The embedded
binary-log core trim saves 72,232 bytes and one archive member. Omitting
`PROCEDURE ANALYSE()` saves 39,120 bytes with no member-count change because
the implementation object is replaced by a small stub. Omitting
system-variable help text saves 56,040 bytes with no member-count change.
Omitting static `SHOW` information saves 32,936 bytes with no member-count
change. The final archive is 4,392,072 bytes smaller than the Release build
with Performance Schema disabled, 5,992,008 bytes smaller than the
symbol-stripped baseline that still built Performance Schema, and 6,704,688
bytes smaller than the original broad archive.

The build found system OpenSSL 3.6.2, bundled zlib, Curses, CURL, LibXml2,
GSSAPI, BZip2, LZ4, LibLZMA, LZO, PCRE2, and Zstandard support on this
machine.

## Enabled Surface

The profile leaves most MariaDB defaults intact. Apart from packaging-only
symbol stripping, the embedded archive includes the static embedded server
library and static embedded engines/plugins such as:

- Aria
- CSV
- HEAP/MEMORY
- InnoDB
- MyISAM and MRG_MyISAM
- Sequence and partition support
- selected static server plugins such as auth socket, type handlers, user
  variables, userstat, and thread-pool info

Configure also enables many module targets, including Archive, Blackhole,
CONNECT, Example, Federated, FederatedX, Mroonga, Sphinx, Spider, and many
server plugins. The `libmariadbd.a` target does not build every configured
module, but the enabled list is still important size-profile evidence because
future profile hardening should disable unwanted surfaces intentionally.
Performance Schema is not part of the default embedded archive; the
server-surface policy treats it as either omitted by the build profile or
disabled when a custom build includes it. `HELP` is present only as a small
unsupported-command shim in the embedded archive. Statement profiling reports
`@@have_profiling=NO` and top-level profiling commands are rejected by policy.
Query-cache implementation code is stubbed for the embedded archive; `SQL_CACHE`
and `SQL_NO_CACHE` remain accepted parser hints, while query-cache management
commands and variables are rejected by policy. Oracle SQL mode is rejected by
policy and linked to an unsupported parser stub; normal MariaDB/MySQL parsing
continues to use the generated MariaDB parser. `SFORMAT()` is omitted from the
embedded function registry, while ordinary `FORMAT()` remains available.
Dynamic UDF lookup, execution, and registration are omitted; stored functions
remain a separate SQL routine surface.
The active binary-log transaction/event core is disabled in the default
embedded archive. `log.cc` and shared binlog/event symbols remain where generic
MariaDB logging, transaction coordination, or retained parser/runtime code
still reference them; this slice does not claim full event-object removal.
`PROCEDURE ANALYSE()` is omitted from the default embedded archive and linked
to an unsupported stub; ordinary SELECT execution and the generic retained
SELECT procedure dispatch continue to link. System-variable names, values,
defaults, validation, and `SHOW VARIABLES` remain available; only
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty in the default
embedded profile. Static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
`SHOW PRIVILEGES` are omitted from the default embedded archive and rejected by
policy; ordinary supported `SHOW` surfaces such as `SHOW VARIABLES` remain
available.

## Disabled Or Missing Surface

The baseline explicitly disables:

- WSREP/Galera
- Aria S3 support
- Performance Schema
- Feedback reporting
- SQL `HELP` table lookup
- Statement profiling
- Query cache
- Oracle SQL mode
- `SFORMAT()`
- Dynamic UDF shared-library loading
- Active binary-log transaction/event core
- `PROCEDURE ANALYSE()`
- System-variable help text
- Static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES`
  information producers
- MariaDB upstream unit-test targets

Configure also reports unavailable optional features on this host, including
RocksDB, OQGraph, AWS key management, CONNECT JDBC, and Snappy.

## Offline Build Caveat

This profile does not fetch MariaDB submodules, but upstream MariaDB's
`cmake/libfmt.cmake` can still check or configure `fmt` 12.1.0 when a usable
system `fmt` is not configured. The embedded `libmariadbd.a` target no longer
includes or depends on fmt after `SFORMAT()` is omitted, but a future CI/release
slice should still audit the top-level MariaDB configure path if fully offline
builds become a requirement.

## Follow-Up

Use this baseline as the comparison point for later profile changes. Each
future trimming slice should record the same archive path, size, member count,
cache options, and compatibility rationale. Runtime-functionality cuts remain
separate decisions from packaging-only archive stripping.
