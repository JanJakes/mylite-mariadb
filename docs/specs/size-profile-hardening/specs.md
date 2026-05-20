# Size Profile Hardening

## Problem Statement

MyLite should start reducing binary size only where the change does not remove
important MySQL/MariaDB behavior. The first safe steps are packaging hygiene,
size-oriented compilation, and omission of already-disabled server surfaces:
remove debug and local-symbol metadata from the embedded static archive, build
the embedded archive with size-oriented release flags, and avoid building the
unused Performance Schema and Feedback static plugins. Server help-table
lookup, statement profiling, and the query cache are also compiled to disabled
surfaces because they do not belong to the embedded application SQL profile.
The optional Oracle SQL-mode parser is treated the same way after policy
coverage proves attempts to enable `sql_mode=ORACLE` fail explicitly. The
fmtlib-backed `SFORMAT()` helper is also omitted from the embedded profile so
the embedded SQL target can build without C++ exceptions; ordinary `FORMAT()`
and core string functions remain available. The same exception-free target also
omits non-semantic unwind tables. Dynamic UDF shared-library loading is omitted
because it is a server-owned extension surface, not core embedded application
behavior.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Current measured archive before this slice:
  `build/mariadb-embedded/libmysqld/libmariadbd.a`, 33,842,320 bytes,
  32.27 MiB, 822 members.
- The largest original archive members include charset/collation tables,
  generated parsers, core SQL item files, JSON, GEOMETRY/GIS, and native engine
  code. The generated MariaDB parser remains compatibility-critical. The
  generated Oracle parser is different: it serves optional Oracle SQL mode,
  which is outside the embedded MySQL/MariaDB application profile.
- Performance Schema accounts for about 1.28 MiB and 112 archive members in
  the symbol-stripped archive. It is already outside the default embedded
  profile, can be disabled at startup when present, and is covered by
  server-surface tests as omitted or disabled.
- Historical bundle-size research shows archive symbol stripping as a
  packaging-only reduction that passed relinked smokes. The old `strip -g`
  command is GNU-specific; Apple `strip` accepts `-S -x` for debug/local-symbol
  stripping.
- On the current macOS baseline, `strip -S -x` plus `ranlib` on a copy of
  `libmariadbd.a` reduces the archive by 712,680 bytes without changing archive
  membership.
- Setting `PLUGIN_PERFSCHEMA=NO` and keeping archive stripping enabled reduces
  the current archive to 31,529,704 bytes, 30.07 MiB, and 712 members.
- Building the same profile with `CMAKE_BUILD_TYPE=MinSizeRel` reduces the
  stripped archive to 30,403,000 bytes, 28.99 MiB, and 712 members.
- Disabling the Feedback plugin removes telemetry/reporting code from the
  embedded archive and reduces the stripped archive to 30,359,112 bytes,
  28.95 MiB, and 707 members.
- Replacing embedded `HELP` execution with unsupported-command stubs reduces
  the stripped archive to 30,296,952 bytes, 28.89 MiB, and 707 members.
- Disabling statement profiling with MariaDB's `ENABLED_PROFILING=OFF` switch
  reduces the stripped archive to 30,228,928 bytes, 28.83 MiB, and 707
  members. The stripped `sql_profile.cc.o` member drops from 48,152 bytes to
  7,376 bytes while preserving MariaDB's `@@have_profiling=NO` contract.
- Replacing the embedded query-cache implementation with no-op stubs reduces
  the stripped archive to 30,188,592 bytes, 28.79 MiB, and 707 members. The
  stripped `sql_cache.cc.o` member drops from 34,968 bytes to 5,368 bytes and
  `emb_qcache.cc.o` drops from 7,168 bytes to 320 bytes while preserving
  no-op `SQL_CACHE` / `SQL_NO_CACHE` parser hints and the
  `@@have_query_cache=NO` contract.
- Replacing the generated embedded Oracle parser with an unsupported stub
  reduces the stripped archive to 29,244,456 bytes, 27.89 MiB, and 707
  members. The stripped `yy_oracle.cc.o` member is replaced by a 784-byte
  `mylite_oracle_parser_stub.cc.o` member.
- `SFORMAT()` is a MariaDB-specific fmtlib-backed formatting helper implemented
  in `mariadb/sql/item_strfunc.cc`, registered from
  `mariadb/sql/item_create.cc`, and protected by a `fmt::format_error`
  `try`/`catch`. It is not core MySQL/MariaDB application behavior, while the
  ordinary numeric `FORMAT()` SQL function remains separate and supported.
- Omitting embedded `SFORMAT()` removes the embedded target's fmt include and
  dependency and allows `sql_embedded` to compile with `-fno-exceptions`. That
  reduces the stripped archive to 27,436,216 bytes, 26.17 MiB, and 707
  members. The stripped `item_strfunc.cc.o` member is 497,216 bytes and
  `item_create.cc.o` is 341,768 bytes in the resulting archive.
- Adding `-fno-asynchronous-unwind-tables` and `-fno-unwind-tables` to the same
  exception-free embedded SQL target reduces the stripped archive to 27,425,376
  bytes, 26.15 MiB, and 707 members.
- MariaDB dynamic UDF support is implemented by `mariadb/sql/sql_udf.cc`,
  `udf_handler` execution paths in `mariadb/sql/item_func.cc` and
  `mariadb/sql/item_sum.cc`, and parser/function-builder hooks in
  `mariadb/sql/sql_yacc.yy` and `mariadb/sql/item_create.cc`. UDF registration
  uses `CREATE FUNCTION ... SONAME`, loads shared libraries from the server
  plugin directory, and updates the `mysql.func` system table.
- Omitting embedded dynamic UDF lookup, execution, and DDL runtime removes
  `sql_udf.cc.o`, reduces the stripped archive to 27,337,960 bytes,
  26.07 MiB, and 706 members, and leaves stored functions as a separate
  application SQL surface.

## Proposed Design

After building the embedded archive, `tools/mariadb-embedded-build` strips
debug and local symbols from `libmariadbd.a` and refreshes the archive index
with `ranlib`.

The embedded baseline uses `CMAKE_BUILD_TYPE=MinSizeRel` so MariaDB compiles
the same runtime surface with size-oriented release flags.

The embedded baseline also disables the Performance Schema storage engine at
configure time. The runtime only passes `--performance-schema=OFF` when the
MariaDB build exposes that option, preserving the explicit disabled
server-surface contract for custom builds while avoiding the unused static
Performance Schema archive members in the default profile.

The embedded baseline also disables MariaDB's Feedback plugin at configure
time. Feedback is a server reporting surface, not SQL, type, or storage-engine
functionality, so omitting it removes low-value embedded code without changing
the supported runtime contract.

The embedded `sql_help.cc` build keeps only small unsupported-command stubs.
MyLite rejects `HELP` in the SQL policy before dispatch, and the MariaDB stubs
preserve fail-closed behavior if the policy is bypassed.

The embedded baseline disables statement profiling with `ENABLED_PROFILING=OFF`.
MyLite rejects top-level `SHOW PROFILE`, `SHOW PROFILES`, and profiling
variable assignment before dispatch so profiling remains unsupported even if a
custom MariaDB build enables the upstream profiling code.

The embedded query-cache implementation is compiled to no-op stubs. MyLite
keeps `SQL_CACHE` and `SQL_NO_CACHE` as accepted parser hints, reports
`@@have_query_cache=NO`, and rejects query-cache management commands and
variables before dispatch.

The embedded archive links a MyLite-owned Oracle parser stub instead of the
generated `yy_oracle.cc` parser. MyLite rejects attempts to enable
`sql_mode=ORACLE` before dispatch, while normal SQL modes and user variables
named `sql_mode` continue through the generated MariaDB parser.

The embedded SQL function registry omits `SFORMAT()` and the embedded
`item_strfunc.cc` build excludes the fmtlib-backed implementation. With the
exception-using implementation absent, `sql_embedded` is compiled with
`-fno-exceptions`. Non-embedded MariaDB targets keep the upstream `SFORMAT()`
implementation.

The same embedded SQL target uses `-fno-asynchronous-unwind-tables` and
`-fno-unwind-tables` to omit non-semantic unwind metadata. This is scoped to the
exception-free target, not applied globally.

The embedded archive omits dynamic UDF runtime by compiling the embedded SQL
target with `MYLITE_WITH_UDF_RUNTIME=0` and excluding `sql_udf.cc`. The parser
and function builders keep upstream behavior outside the embedded profile.
MyLite rejects `CREATE FUNCTION ... SONAME` before dispatch so no UDF shared
library or `mysql.func` metadata path is exposed through the core API.

The wrapper keeps this behavior enabled by default because it is the
distributed archive profile. Developers can set `STRIP_ARCHIVE=0` when they
need an unstripped archive for local inspection.

## Affected MariaDB Subsystems

The Performance Schema storage-engine plugin and Feedback reporting plugin are
omitted by CMake configuration, embedded `HELP`, statement profiling, query
cache, Oracle SQL mode, and `SFORMAT()` are compiled to disabled or omitted
surfaces, and the compiled objects use size-oriented release flags. The
embedded SQL target also uses `-fno-exceptions`, omits unwind tables, and omits
dynamic UDF lookup, execution, and DDL runtime.

## Compatibility Impact

No application compatibility impact is expected. This slice does not remove SQL
syntax, functions, data types, collations, supported storage engines,
diagnostics, or public C API behavior. Performance Schema remains outside the
core embedded profile, and Feedback reporting is not part of the embedded
runtime contract. SQL `HELP` is a server help-table surface and is explicitly
unsupported in the embedded profile. Statement profiling is a diagnostic
server surface, not application data behavior, and is explicitly unsupported in
the embedded profile. Query-cache management is a server-side result-cache
optimization; MyLite keeps query-cache SELECT hints as no-op syntax and omits
the cache implementation. Oracle SQL mode is an optional MariaDB compatibility
mode, not core MySQL/MariaDB application behavior; MyLite rejects it explicitly
and keeps the normal MariaDB parser intact.
`SFORMAT()` is an optional fmtlib-backed helper rather than core application
SQL behavior; ordinary `FORMAT()` remains available, and direct or prepared
`SFORMAT()` fails predictably in the default embedded profile.
Omitting unwind tables from the exception-free embedded SQL target has no SQL,
storage, public API, or diagnostics impact.
Dynamic UDF registration is a server extension surface based on shared-library
loading and `mysql.func` metadata. MyLite rejects `CREATE FUNCTION ... SONAME`
directly and in prepared statements; stored functions and built-in SQL
functions are not removed by this slice.

## Database-Directory And Lifecycle Impact

None. Runtime directory layout, storage files, temporary files, and lock
behavior are unchanged.

## Public API Impact

None. `libmylite` headers and symbols are unchanged.

## Native Storage Impact

None. InnoDB, MyISAM, Aria, and MEMORY coverage should continue to link and
run against the same native engine members.

## Binary-Size Impact

The first step is archive-only: 712,680 bytes from debug/local-symbol
stripping. Disabling Performance Schema removes unused static plugin members.
Switching the same profile to `MinSizeRel`, omitting Feedback, and compiling
embedded `HELP` to stubs brings the archive to 30,296,952 bytes / 28.89 MiB.
Disabling statement profiling brings the current archive to 30,228,928 bytes /
28.83 MiB, 1,300,776 bytes smaller than the Release build with Performance
Schema disabled and 2,900,712 bytes smaller than the symbol-stripped baseline
with Performance Schema still built. Stubbing the embedded query cache brings
the current archive to 30,188,592 bytes / 28.79 MiB, 1,341,112 bytes smaller
than the Release build with Performance Schema disabled and 2,941,048 bytes
smaller than the symbol-stripped baseline with Performance Schema still built.
Replacing the generated Oracle parser with an unsupported stub brings the
current archive to 29,244,456 bytes / 27.89 MiB, 2,285,248 bytes smaller than
the Release build with Performance Schema disabled, 3,885,184 bytes smaller
than the symbol-stripped baseline with Performance Schema still built, and
4,597,864 bytes smaller than the original broad archive. Omitting embedded
`SFORMAT()` and compiling the embedded SQL target with `-fno-exceptions` brings
the current archive to 27,436,216 bytes / 26.17 MiB, 4,093,488 bytes smaller
than the Release build with Performance Schema disabled, 5,693,424 bytes
smaller than the symbol-stripped baseline with Performance Schema still built,
and 6,406,104 bytes smaller than the original broad archive. Omitting unwind
tables from the same exception-free target brings the current archive to
27,425,376 bytes / 26.15 MiB, 4,104,328 bytes smaller than the Release build
with Performance Schema disabled, 5,704,264 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 6,416,944
bytes smaller than the original broad archive. Omitting dynamic UDF runtime
brings the current archive to 27,337,960 bytes / 26.07 MiB, 4,191,744 bytes
smaller than the Release build with Performance Schema disabled, 5,791,680
bytes smaller than the symbol-stripped baseline with Performance Schema still
built, and 6,504,360 bytes smaller than the original broad archive.

## License Or Dependency Impact

No new dependencies or license changes. The wrapper uses standard `strip` and
`ranlib` tools already expected in the native build toolchain.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Run `cmake --build --preset dev`.
- Run `ctest --preset dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev`.
- Run `ctest --preset embedded-dev --output-on-failure`.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `cmake --build --preset dev --target tidy`.
- Run `cmake --build --preset embedded-dev --target tidy`.
- Run `git diff --check`.
- Run `tools/mariadb-embedded-build measure`.

## Acceptance Criteria

- The embedded build wrapper produces a stripped `libmariadbd.a` by default.
- `STRIP_ARCHIVE=0` preserves an unstripped archive for diagnostics.
- The embedded archive builds with size-oriented release flags.
- Performance Schema is omitted from the embedded archive and remains omitted
  or disabled at runtime.
- Feedback reporting is omitted from the embedded archive.
- SQL `HELP` fails through the MyLite policy and the embedded MariaDB stub.
- Statement profiling is disabled in the embedded archive and profiling SQL
  fails through the MyLite policy.
- Query cache reports unavailable, query-cache management fails through the
  MyLite policy, and query-cache SELECT hints remain no-op syntax.
- Oracle SQL mode fails through the MyLite policy and the embedded parser stub.
- Embedded `SFORMAT()` is omitted, direct and prepared `SFORMAT()` fail
  predictably, and ordinary `FORMAT()` remains available.
- The embedded SQL target builds with `-fno-exceptions` and without unwind
  tables.
- Dynamic UDF registration through `CREATE FUNCTION ... SONAME` fails through
  MyLite policy, and the embedded archive omits UDF lookup, execution, and DDL
  runtime.
- The stripped archive still links `libmylite` and all embedded tests.
- The measured archive size and member count are recorded in the build
  documentation.
- Compatibility documentation records unsupported server diagnostic surfaces.

## Risks And Unresolved Questions

- Stripping local symbols reduces archive-level debugging and postmortem
  symbol inspection. Developers can rebuild with `STRIP_ARCHIVE=0` when that
  matters.
- Larger size wins require removing or stubbing code. Those changes need
  separate compatibility decisions before they are accepted.
- Compiling the embedded SQL target without exceptions is valid only while
  exception-using SQL surfaces remain outside the embedded profile and covered
  by tests.
- Unwind-table omission should stay scoped to targets where it is non-semantic.
- Stored functions remain planned application SQL. Dynamic UDF policy and size
  trimming must stay scoped to shared-library UDF registration and execution.
