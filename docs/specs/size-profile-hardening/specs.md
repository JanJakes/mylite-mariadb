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
behavior. Binary-log transaction/event runtime is treated the same way after
policy coverage proves replication and binlog SQL are outside the core API.
Legacy `PROCEDURE ANALYSE()` is omitted because it is an obsolete diagnostic
SELECT extension rather than application data behavior. Long system-variable
help comments are omitted because they are descriptive server help text rather
than variable behavior. Static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
`SHOW PRIVILEGES` producers are omitted because they expose server
attribution and privilege help metadata rather than application data.
Command-line option help text is omitted because it is inherited server
documentation prose, while option names and parser metadata remain available.
Optimizer trace diagnostics are omitted because they expose server optimizer
debugging data rather than application behavior. General and slow query logs
are omitted because they are daemon query-audit diagnostics rather than
application storage, SQL execution, or public API behavior.

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
- The embedded baseline starts MariaDB with `--skip-log-bin`, policy-rejects
  replication and binlog command families, and verifies `@@log_bin=0` plus
  absent binlog/relay-log sidecars. `mariadb/sql/log.cc`,
  `mariadb/sql/handler.cc`, `mariadb/sql/sql_class.cc`, and
  `mariadb/sql/sql_builtin.cc.in` still retain binlog transaction, row-event,
  GTID-state, and mandatory plugin entry points unless guarded.
- Disabling the embedded binary-log core behind `MYLITE_WITH_BINLOG_CORE=0`
  removes `rpl_record.cc.o`, compiles supported no-op/fail-closed entry points,
  and reduces the stripped archive to 27,265,728 bytes, 26.00 MiB, and 705
  members. Shared log/event symbols remain where retained MariaDB code still
  references them.
- `mariadb/sql/procedure.cc` registers the built-in `analyse` SELECT procedure
  and dispatches it to `proc_analyse_init()`. `mariadb/sql/sql_analyse.cc`
  implements that analyzer and is included in the embedded SQL archive by
  `mariadb/libmysqld/CMakeLists.txt`.
- Disabling `PROCEDURE ANALYSE()` behind
  `MYLITE_WITH_PROCEDURE_ANALYSE=0` replaces `sql_analyse.cc.o` with a small
  unsupported stub and reduces the stripped archive to 27,226,608 bytes,
  25.97 MiB, and 705 members.
- System-variable definitions in `mariadb/sql/sys_vars.cc` pass long
  human-readable comments through `Sys_var_*` constructors in
  `mariadb/sql/sys_vars.inl` into `sys_var::option.comment`. `fill_sysvars()`
  exposes that pointer through
  `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT`, while `SHOW
  VARIABLES`, `INFORMATION_SCHEMA.GLOBAL_VARIABLES`, and
  `INFORMATION_SCHEMA.SESSION_VARIABLES` use variable names and value pointers.
- Disabling system-variable help text behind
  `MYLITE_WITH_SYSVAR_HELP_TEXT=0` removes read-only string data from existing
  objects and reduces the stripped archive to 27,170,568 bytes, 25.91 MiB, and
  705 members.
- `mariadb/sql/sql_yacc.yy` parses `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
  `SHOW PRIVILEGES` into dedicated `SQLCOM_SHOW_*` commands.
  `mariadb/sql/sql_parse.cc` dispatches those commands to
  `mysqld_show_authors()`, `mysqld_show_contributors()`, and
  `mysqld_show_privileges()`.
- `mariadb/sql/sql_show.cc` implements those result producers, including
  static authors and contributors rows from `authors.h` and `contributors.h`,
  plus the `sys_privileges[]` static privilege table.
- Disabling static `SHOW` information behind
  `MYLITE_WITH_STATIC_SHOW_INFO=0` removes those static result producers and
  reduces the stripped archive to 27,137,632 bytes, 25.88 MiB, and 705 members.
- `mariadb/sql/mysqld.cc` defines the server `my_long_options[]` option table.
  Its `my_option::comment` field feeds automated `--help` text through
  `my_print_help()` in `mariadb/mysys/my_getopt.c`; option parsing uses the
  same table's names, variables, types, argument policy, defaults, bounds, and
  aliases.
- Disabling command-line option help text behind
  `MYLITE_WITH_OPTION_HELP_TEXT=0` maps only those hardcoded option comments to
  empty strings and reduces the stripped archive to 27,128,952 bytes,
  25.87 MiB, and 705 members.
- `mariadb/sql/opt_trace.cc` implements optimizer trace collection,
  `INFORMATION_SCHEMA.OPTIMIZER_TRACE` rows, trace security checks, and trace
  JSON helper symbols referenced by retained optimizer paths. The safe embedded
  cut is an inert replacement object, not deleting the header-level trace API.
- Disabling optimizer trace diagnostics behind
  `MYLITE_WITH_OPTIMIZER_TRACE=0` replaces `opt_trace.cc.o` with
  `mylite_opt_trace_disabled.cc.o` and reduces the stripped archive to
  27,116,808 bytes, 25.86 MiB, and 705 members.
- `mariadb/sql/log.cc` owns general and slow query-log dispatch, table
  handlers, file handlers, and `MYSQL_QUERY_LOG` formatting. It also owns
  shared error-log entry points, so the safe cut is inert query-log handlers,
  not removing the shared logging object.
- Disabling general and slow query-log runtime behind
  `MYLITE_WITH_QUERY_LOGS=0` keeps error logging and diagnostics available,
  rejects query-log configuration through the MyLite SQL policy, and reduces
  the stripped archive to 27,095,640 bytes, 25.84 MiB, and 705 members.

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

The embedded archive disables binary-log core runtime by setting
`MYLITE_WITH_BINLOG_CORE=0` in the MyLite baseline, omitting `rpl_record.cc`,
skipping mandatory binlog plugin registration, and compiling binlog
transaction, row-event, GTID-state, event-write, and table-map entry points to
no-op or fail-closed behavior. The option defaults to `ON` so normal MariaDB
server builds keep upstream binlog behavior.

The embedded archive disables `PROCEDURE ANALYSE()` by setting
`MYLITE_WITH_PROCEDURE_ANALYSE=0` in the MyLite baseline, replacing
`sql_analyse.cc` with a small `proc_analyse_init()` unsupported stub. The
option defaults to `ON` so normal MariaDB server builds keep upstream behavior.
MyLite rejects straightforward direct and prepared
`SELECT ... PROCEDURE ANALYSE()` before dispatch, while the MariaDB stub
remains the fail-closed backstop.

The embedded archive omits long system-variable help comments by setting
`MYLITE_WITH_SYSVAR_HELP_TEXT=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream comments.
`mariadb/libmysqld/CMakeLists.txt` defines
`MYLITE_DISABLE_SYSVAR_HELP_TEXT` for the embedded profile, and
`mariadb/sql/sys_vars.inl` maps `MYLITE_SYSVAR_HELP_TEXT(...)` to an empty
string under that macro. `mariadb/sql/sys_vars.cc` wraps system-variable
comment arguments at the declaration site so the long string literals are
discarded before compilation.

The embedded archive omits static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
`SHOW PRIVILEGES` result producers by setting
`MYLITE_WITH_STATIC_SHOW_INFO=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream behavior. MyLite rejects
direct and prepared static `SHOW` information SQL before dispatch, while
`sql_parse.cc` remains the fail-closed backstop if the public policy is
bypassed.

The embedded archive omits hardcoded command-line option help text by setting
`MYLITE_WITH_OPTION_HELP_TEXT=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream `--help` prose. The embedded
profile wraps only `my_long_options[]` comment strings in
`MYLITE_OPTION_HELP_TEXT(...)`; option names, aliases, value pointers, argument
types, defaults, bounds, deprecation aliases, and parsing behavior remain
compiled normally.

The embedded archive omits optimizer trace diagnostics by setting
`MYLITE_WITH_OPTIMIZER_TRACE=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream optimizer trace behavior.
The disabled profile links `mylite_opt_trace_disabled.cc` in place of
`opt_trace.cc`, preserving helper symbols required by retained optimizer code
while never collecting or exposing optimizer trace rows. MyLite rejects
optimizer-trace variable assignment and
`INFORMATION_SCHEMA.OPTIMIZER_TRACE`, qualified or current-schema, before
dispatch; ordinary planning, execution, and `EXPLAIN` remain available.

The embedded archive omits general and slow query-log runtime by setting
`MYLITE_WITH_QUERY_LOGS=0` in the MyLite baseline. The option defaults to `ON`
so normal MariaDB server builds keep upstream query-log behavior. The disabled
profile keeps the shared logger and error-log paths but makes query-log table
handlers, file handlers, writes, flushes, and activation inert. MyLite starts
with `--log-output=NONE` and rejects query-log variable assignment plus
`FLUSH LOGS`, `FLUSH GENERAL LOGS`, and `FLUSH SLOW LOGS`, including `LOCAL`
and `NO_WRITE_TO_BINLOG` variants, before dispatch.

The wrapper keeps this behavior enabled by default because it is the
distributed archive profile. Developers can set `STRIP_ARCHIVE=0` when they
need an unstripped archive for local inspection.

## Affected MariaDB Subsystems

The Performance Schema storage-engine plugin and Feedback reporting plugin are
omitted by CMake configuration, embedded `HELP`, statement profiling, query
cache, Oracle SQL mode, `SFORMAT()`, and `PROCEDURE ANALYSE()` are compiled to
disabled or omitted surfaces, and the compiled objects use size-oriented
release flags. The embedded SQL target also uses `-fno-exceptions`, omits
unwind tables, and omits dynamic UDF lookup, execution, and DDL runtime. The
embedded baseline also disables binlog transaction/event runtime behind a
MyLite-owned profile flag and omits long system-variable help comments from
`sys_vars.cc`. Static server-information `SHOW` producers are compiled out of
`sql_show.cc`. Hardcoded command-line option help strings are compiled out of
the embedded `mysqld.cc` option table. Optimizer trace diagnostics are replaced
with inert trace helper symbols in the embedded SQL archive. General and slow
query-log handlers are compiled to inert embedded paths while shared error-log
behavior remains available.

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
Replication and binary logging are server-topology surfaces. MyLite already
rejects replication and binlog command families, starts with `@@log_bin=0`,
and verifies that no binlog or relay-log sidecars are created. The no-binlog
core keeps supported DDL, DML, transactions, crash/reopen behavior, and native
engine coverage intact.
`PROCEDURE ANALYSE()` is a legacy diagnostic SELECT extension. Omitting it does
not affect ordinary SELECT execution, DDL, DML, native storage engines, JSON,
GEOMETRY/GIS, or the public C API.
System-variable help comments are descriptive metadata. Omitting them leaves
system variables, values, defaults, validation, `SHOW VARIABLES`,
`INFORMATION_SCHEMA.GLOBAL_VARIABLES`, and
`INFORMATION_SCHEMA.SESSION_VARIABLES` available. The only SQL-visible
difference is that
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty in the default
embedded profile.
Static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` are
server-information and privilege-help surfaces. Omitting them leaves ordinary
supported `SHOW` surfaces such as `SHOW VARIABLES`, table metadata inspection,
and warnings available.
Command-line option help text is documentation metadata. Omitting it leaves
embedded startup option parsing, names, aliases, types, defaults, and limits
available; inherited `--help` descriptions are empty in the embedded archive.
Optimizer trace is a server diagnostic surface. Omitting it removes optimizer
trace output and `INFORMATION_SCHEMA.OPTIMIZER_TRACE`, not ordinary query
planning, execution, `EXPLAIN`, JSON functions, native storage engines, DDL,
DML, or the public C API.
General and slow query logs are daemon diagnostics that write statement text to
server log files or log tables. Omitting them removes query-log output and
configuration, not ordinary statement execution, SQL diagnostics, errors,
warnings, native storage engines, DDL, DML, or the public C API.

## Database-Directory And Lifecycle Impact

Runtime directory layout, storage files, temporary files, and lock behavior are
unchanged. Query-log omission removes inherited daemon log-file output rather
than adding any database-directory companions.

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
built, and 6,504,360 bytes smaller than the original broad archive. Disabling
the embedded binary-log core brings the current archive to 27,265,728 bytes /
26.00 MiB, 4,263,976 bytes smaller than the Release build with Performance
Schema disabled, 5,863,912 bytes smaller than the symbol-stripped baseline with
Performance Schema still built, and 6,576,592 bytes smaller than the original
broad archive.
Omitting `PROCEDURE ANALYSE()` brings the current archive to 27,226,608 bytes /
25.97 MiB, 4,303,096 bytes smaller than the Release build with Performance
Schema disabled, 5,903,032 bytes smaller than the symbol-stripped baseline with
Performance Schema still built, and 6,615,712 bytes smaller than the original
broad archive.
Omitting system-variable help text brings the current archive to 27,170,568
bytes / 25.91 MiB, 4,359,136 bytes smaller than the Release build with
Performance Schema disabled, 5,959,072 bytes smaller than the symbol-stripped
baseline with Performance Schema still built, and 6,671,752 bytes smaller than
the original broad archive.
Omitting static `SHOW` information brings the current archive to 27,137,632
bytes / 25.88 MiB, 4,392,072 bytes smaller than the Release build with
Performance Schema disabled, 5,992,008 bytes smaller than the symbol-stripped
baseline with Performance Schema still built, and 6,704,688 bytes smaller than
the original broad archive.
Omitting command-line option help text brings the current archive to 27,128,952
bytes / 25.87 MiB, 4,400,752 bytes smaller than the Release build with
Performance Schema disabled, 6,000,688 bytes smaller than the symbol-stripped
baseline with Performance Schema still built, and 6,713,368 bytes smaller than
the original broad archive.
Omitting optimizer trace diagnostics brings the current archive to 27,116,808
bytes / 25.86 MiB, 4,412,896 bytes smaller than the Release build with
Performance Schema disabled, 6,012,832 bytes smaller than the symbol-stripped
baseline with Performance Schema still built, and 6,725,512 bytes smaller than
the original broad archive.
Omitting general and slow query-log runtime brings the current archive to
27,095,640 bytes / 25.84 MiB, 4,434,064 bytes smaller than the Release build
with Performance Schema disabled, 6,034,000 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 6,746,680
bytes smaller than the original broad archive.

## License Or Dependency Impact

No new dependencies or license changes. The wrapper uses standard `strip` and
`ranlib` tools already expected in the native build toolchain.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `sql_analyse.cc.o` is absent and
  `mylite_procedure_analyse_stub.cc.o` is present in `libmariadbd.a`.
- Confirm `opt_trace.cc.o` is absent and `mylite_opt_trace_disabled.cc.o` is
  present in `libmariadbd.a`.
- Confirm `MYLITE_WITH_QUERY_LOGS=OFF` appears in the embedded CMake cache and
  query-log configuration SQL is rejected by server-surface policy coverage.
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
- Replication and binlog command families remain rejected, `@@log_bin=0`
  remains covered, no binlog/relay-log sidecars are created, and the embedded
  archive omits the active binlog transaction/event core.
- Direct and prepared `SELECT ... PROCEDURE ANALYSE()` fail predictably, quoted
  literal mentions remain normal SQL, and the embedded archive omits
  `sql_analyse.cc.o`.
- System-variable rows and values remain queryable, and
  `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty in the
  default embedded profile.
- Direct and prepared `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
  `SHOW PRIVILEGES` fail through MyLite policy, while ordinary `SHOW
  VARIABLES` remains available.
- Embedded startup option parsing remains covered by the embedded test suite,
  and inherited option help strings are absent from the measured archive.
- Direct and prepared optimizer-trace SQL fails through MyLite policy,
  ordinary `EXPLAIN` remains available, and the embedded archive replaces
  `opt_trace.cc.o` with `mylite_opt_trace_disabled.cc.o`.
- Direct and prepared query-log configuration SQL fails through MyLite policy,
  `@@general_log`, `@@slow_query_log`, and `@@log_output` show the disabled
  embedded state, and error logging remains available.
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
- `log.cc`, `log_event.cc`, GTID helpers, and binlog plugin symbols still have
  shared references. Removing more binlog/event code needs separate source and
  link evidence rather than file-name pruning.
- The generic SELECT procedure dispatch remains linked after omitting
  `PROCEDURE ANALYSE()`. Removing it should be a separate slice.
- Clients that build help UIs from
  `INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` lose those comments
  in the default embedded profile. Variable rows and values remain available.
- Clients that display static server attribution or privilege help from
  `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, or `SHOW PRIVILEGES` lose those
  commands in the default embedded profile.
- Users inspecting MariaDB-style command-line help from the embedded archive
  lose inherited prose descriptions. The option parser still accepts the
  retained embedded startup options.
- Users inspecting MariaDB optimizer trace diagnostics lose
  `INFORMATION_SCHEMA.OPTIMIZER_TRACE` output in the default embedded profile.
  Normal query execution and `EXPLAIN` remain available.
- Users relying on MariaDB general or slow query logs lose those daemon
  diagnostics in the default embedded profile. MyLite still exposes SQL errors,
  warnings, result metadata, and normal statement diagnostics through the C API.
