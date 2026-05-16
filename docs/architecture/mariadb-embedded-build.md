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
WITH_ZLIB=none
PLUGIN_S3=NO
WITHOUT_DYNAMIC_PLUGINS=ON
ENABLED_PROFILING=OFF
MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=OFF
MYLITE_WITH_LOAD_DATA=OFF
MYLITE_WITH_SQL_FILE_IO=OFF
MYLITE_WITH_SERVER_UTILITY_FUNCTIONS=OFF
MYLITE_WITH_ORACLE_SQL_MODE=OFF
MYLITE_WITH_XML_SQL_FUNCTIONS=OFF
MYLITE_WITH_GIS_SQL_FUNCTIONS=OFF
MYLITE_WITH_SFORMAT_SQL_FUNCTION=OFF
MYLITE_WITH_JSON_SCHEMA_VALID=OFF
MYLITE_WITH_JSON_TABLE=OFF
MYLITE_WITH_DYNAMIC_COLUMNS=OFF
MYLITE_WITH_SQL_HANDLER_COMMAND=OFF
MYLITE_WITH_SEQUENCE_RUNTIME=OFF
MYLITE_WITH_HELP_COMMAND=OFF
MYLITE_WITH_PROCEDURE_ANALYSE=OFF
MYLITE_WITH_SELECT_PROCEDURE_RUNTIME=OFF
MYLITE_WITH_STORED_PROGRAM_RUNTIME=OFF
MYLITE_WITH_EVENT_PARSE_DATA_VALIDATION=OFF
MYLITE_WITH_TRIGGER_RUNTIME=OFF
MYLITE_WITH_VIEW_RUNTIME=OFF
MYLITE_WITH_ZLIB_COMPRESSION=OFF
MYLITE_WITH_UDF_RUNTIME=OFF
MYLITE_WITH_BINLOG_CORE=OFF
MYLITE_WITH_MYISAM_MAINTENANCE=OFF
MYLITE_WITH_FOREIGN_SERVER_METADATA=OFF
MYLITE_WITH_BACKUP_RUNTIME=OFF
MYLITE_WITH_QUERY_CACHE_RUNTIME=OFF
MYLITE_WITH_OPTIMIZER_TRACE=OFF
MYLITE_WITH_STATIC_SHOW_INFO=OFF
MYLITE_WITH_STATUS_METADATA=OFF
MYLITE_WITH_PROCESSLIST_METADATA=OFF
MYLITE_WITH_ROUTINE_METADATA=OFF
MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS=OFF
PLUGIN_AUTH_SOCKET=NO
PLUGIN_FEEDBACK=NO
PLUGIN_PERFSCHEMA=NO
PLUGIN_SEQUENCE=NO
PLUGIN_THREAD_POOL_INFO=NO
PLUGIN_USERSTAT=NO
```

`WITH_WSREP=OFF` and `PLUGIN_S3=NO` are required because the initial MariaDB
import intentionally omits `wsrep-lib` and `storage/maria/libmarias3`.
Dynamic plugin loading, LOAD file import, SQL host-file I/O, server utility SQL
functions, Oracle SQL mode parsing, XML SQL functions, GIS SQL functions, the
MariaDB-specific `SFORMAT()` SQL function, JSON schema validation, the
`JSON_TABLE` table-function runtime, dynamic-column packed BLOB runtime, SQL
`HANDLER` command runtime, SQL sequence runtime, virtual `SEQUENCE` storage
engine, SQL `HELP`,
`PROCEDURE ANALYSE()`, generic SELECT procedure runtime, stored-program runtime,
event parse-data validation, file-backed view runtime and metadata helpers,
file-backed trigger runtime and metadata helpers, dynamic UDF lookup/execution,
binary-log transaction,
event-write, and event-root core, native MyISAM table-maintenance and key-cache
administration,
foreign-server metadata cache, socket authentication, feedback, Performance
Schema, thread-pool info, the user-statistics plugin, external backup-tool
SQL runtime, the server-global query cache runtime, statement profiling,
optimizer trace diagnostics, static SHOW information producers, status
metadata producers, process-list metadata producers, and zlib-backed SQL,
protocol, binlog, column, and InnoDB page compression are disabled
because they are server-administration, blocking utility, Oracle
compatibility, legacy XML helper, spatial-function, MariaDB-specific
formatting, schema validation, table-function projection, packed semi-structured
BLOB handling, direct storage-engine cursor, unsupported sequence object/value
state, help-table lookup, result-set analysis, SELECT result-set extension hook,
catalog-bypassing generated virtual tables, unsupported non-table objects,
view and trigger sidecar metadata, dynamic extension, server topology,
engine-file maintenance, or server/client file, foreign-server metadata,
server-observability, external physical backup, server-global result-cache,
session profiling, optimizer diagnostics, static server-metadata, server status,
process/session introspection, external compression, or host dynamic-loader
surfaces, not core
MyLite embedded runtime behavior. Routine metadata scans of `mysql.proc` are
also disabled until MyLite has a catalog-backed routine design.
The retained `sql_embedded` C++ sources are
also compiled with
`-fno-exceptions`; the flag is not applied to first-party MyLite code or to all
MariaDB targets.

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
| Archive size | 26,776,960 bytes / 25.54 MiB |
| Archive members | 670 |

The build found system OpenSSL 3.6.2, Curses, CURL, GSSAPI, BZip2, LZ4,
LibLZMA, LZO, PCRE2, and Zstandard support on this machine. MariaDB server
zlib compression is disabled with `WITH_ZLIB=none`; Connector/C configure
metadata still reports its private bundled zlib plugin, but the embedded
archive and linked first-party embedded smoke binaries do not link libz.

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

The dynamic-column trim reduced the default archive by a further 29,752 bytes
with the same member count. The disabled profile now replaces `ma_dyncol.c`
with MyLite disabled API stubs, rejects direct and prepared dynamic-column SQL
functions before MariaDB execution, and leaves ordinary BLOB values available.

The SQL HANDLER command trim reduced the default archive by a further 13,424
bytes with the same member count. The disabled profile now replaces
`sql_handler.cc` with a MyLite disabled SQL-handler stub, rejects direct and
prepared top-level `HANDLER` commands before MariaDB execution, and leaves
ordinary SQL table scans and indexed DML available.

The sequence runtime trim reduced the default archive by a further 101,152
bytes and removed two archive members. The disabled profile now replaces
`sql_sequence.cc` and `ha_sequence.cc` with a MyLite disabled sequence stub,
rejects direct and prepared sequence value surfaces before MariaDB execution,
and leaves ordinary `AUTO_INCREMENT` available.

The virtual sequence storage-engine trim reduced the default archive by a
further 45,976 bytes and removed one archive member. The disabled profile now
sets `PLUGIN_SEQUENCE=NO`, omits `sequence.cc`, stops advertising `SEQUENCE`
through `SHOW ENGINES`, and leaves ordinary catalog-backed tables, including
tables whose names resemble `seq_1_to_10`, under MyLite storage routing.

The user-statistics plugin trim reduced the default archive by a further
19,848 bytes and removed one archive member. The disabled profile now sets
`PLUGIN_USERSTAT=NO`, omits `userstat.cc`, rejects direct and prepared
user-statistics `SHOW`, `FLUSH`, `userstat` system-variable assignment, and
`INFORMATION_SCHEMA` statistics-table surfaces before MariaDB execution, and
leaves ordinary SQL user variables available.

The foreign-server metadata trim reduced the default archive by a further
16,784 bytes with the same member count. The disabled profile now sets
`MYLITE_WITH_FOREIGN_SERVER_METADATA=OFF`, replaces `sql_servers.cc` with
`mylite_sql_servers_disabled.cc`, prevents embedded startup from reading
`mysql.servers`, rejects direct and prepared `CREATE SERVER`,
`CREATE OR REPLACE SERVER`, `ALTER SERVER`, `DROP SERVER`, and
`SHOW CREATE SERVER` before MariaDB execution, and leaves ordinary supported
engine routing unchanged.

The backup runtime trim reduced the default archive by a further 7,488 bytes
with the same member count. The disabled profile now sets
`MYLITE_WITH_BACKUP_RUNTIME=OFF`, replaces `backup.cc` with
`mylite_backup_disabled.cc`, rejects direct and prepared `BACKUP STAGE`,
`BACKUP LOCK`, and `BACKUP UNLOCK` before MariaDB execution, and keeps ordinary
DDL and copy `ALTER` backup-hook entry points as no-ops.

The query cache trim reduced the default archive by a further 49,136 bytes and
removed one archive member. The disabled profile now sets
`MYLITE_WITH_QUERY_CACHE_RUNTIME=OFF`, replaces `sql_cache.cc` with
`mylite_query_cache_disabled.cc`, omits `emb_qcache.cc`, reports
`have_query_cache=NO`, rejects direct and prepared `FLUSH QUERY CACHE`,
`RESET QUERY CACHE`, and query-cache system-variable assignment before MariaDB
execution, and leaves ordinary `SELECT`, `SELECT SQL_CACHE`, and
`SELECT SQL_NO_CACHE` available as uncached execution paths.

The statement profiling trim reduced the default archive by a further 56,960
bytes with the same member count. The disabled profile now sets
`ENABLED_PROFILING=OFF`, reports `have_profiling=NO`, rejects direct and
prepared `SHOW PROFILE`, `SHOW PROFILES`, profiling system-variable
assignment, and `INFORMATION_SCHEMA.PROFILING` access before MariaDB
execution, and leaves ordinary `SHOW VARIABLES` and SQL execution available.

The optimizer trace trim reduced the default archive by a further 11,056 bytes
with the same member count. The disabled profile now sets
`MYLITE_WITH_OPTIMIZER_TRACE=OFF`, replaces `opt_trace.cc` with
`mylite_opt_trace_disabled.cc`, rejects direct and prepared optimizer-trace
system-variable assignment and `INFORMATION_SCHEMA.OPTIMIZER_TRACE` access
before MariaDB execution, and leaves ordinary planning, `EXPLAIN`, and SQL
execution paths available with inert trace helpers.

The static SHOW information trim reduced the default archive by a further
37,160 bytes with the same member count. The disabled profile now sets
`MYLITE_WITH_STATIC_SHOW_INFO=OFF`, compiles out static `SHOW AUTHORS`,
`SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` result producers, rejects direct and
prepared access before MariaDB execution, and leaves ordinary supported
`SHOW VARIABLES` and diagnostic surfaces available.

The status metadata trim reduced the default archive by a further 24,144 bytes
with the same member count. The disabled profile now sets
`MYLITE_WITH_STATUS_METADATA=OFF`, compiles out the large `SHOW STATUS`
publication table and dynamic status-variable registry, leaves
`INFORMATION_SCHEMA.GLOBAL_STATUS` and `INFORMATION_SCHEMA.SESSION_STATUS`
visible with zero rows, and makes `SHOW STATUS`, `SHOW GLOBAL STATUS`, and
`SHOW SESSION STATUS` return empty result sets while ordinary `SHOW VARIABLES`
remains available.

The process-list metadata trim reduced the default archive by a further
40,712 bytes with the same member count. The disabled profile now sets
`MYLITE_WITH_PROCESSLIST_METADATA=OFF`, compiles out the `SHOW PROCESSLIST`
and `INFORMATION_SCHEMA.PROCESSLIST` row producers, rejects direct and prepared
`SHOW PROCESSLIST` and `SHOW FULL PROCESSLIST` before MariaDB execution, and
leaves `INFORMATION_SCHEMA.PROCESSLIST` visible with zero rows.

The routine metadata trim reduced the default archive by a further 14,208
bytes with the same member count. The disabled profile now sets
`MYLITE_WITH_ROUTINE_METADATA=OFF`, compiles out the `mysql.proc` scan helpers
behind `INFORMATION_SCHEMA.ROUTINES` and `INFORMATION_SCHEMA.PARAMETERS`,
leaves those schema tables visible with zero rows, and makes
`SHOW PROCEDURE STATUS` and `SHOW FUNCTION STATUS` return empty result sets
while routine objects remain unsupported.

The trigger runtime trim reduced the default archive by 37,712 bytes from the
status metadata baseline with the same member count. The disabled profile now
sets `MYLITE_WITH_TRIGGER_RUNTIME=OFF`, replaces `sql_trigger.cc` with
`mylite_sql_trigger_disabled.cc`, keeps trigger DDL rejected before MariaDB
execution, leaves `SHOW TRIGGERS` and `INFORMATION_SCHEMA.TRIGGERS` visible
with zero rows, and makes table open, DROP, RENAME, ALTER, and DML paths behave
as if no triggers are present.

The view runtime trim reduced the default archive by 22,576 bytes from the
trigger runtime baseline with the same member count. The disabled profile now
sets `MYLITE_WITH_VIEW_RUNTIME=OFF`, replaces `sql_view.cc` with
`mylite_sql_view_disabled.cc`, keeps view DDL rejected before MariaDB
execution, leaves `INFORMATION_SCHEMA.VIEWS` visible with zero rows, and
preserves the derived-table and CTE projection-name helpers that are still
needed outside persistent views.

The zlib compression trim reduced the default archive by 43,656 bytes from the
view runtime baseline with the same member count. The disabled profile now
sets `MYLITE_WITH_ZLIB_COMPRESSION=OFF` and `WITH_ZLIB=none`, reports
`have_compress=NO`, returns `NULL` from `COMPRESS()` and `UNCOMPRESS()`,
rejects compressed column DDL with an explicit unsupported diagnostic, compiles
retained binlog and InnoDB zlib compression branches as fail-closed no-zlib
paths, and makes first-party linked embedded smoke binaries stop linking
`/usr/lib/libz.1.dylib`.

The dynamic plugin loading trim reduced the default archive by a further
16,104 bytes with the same member count. The disabled profile now sets
`MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=OFF`, clears `HAVE_DLOPEN`,
`HAVE_DLADDR`, `HAVE_DLERROR`, and `HAVE_DLFCN_H` in the generated embedded
configuration, emits `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=0` so platform
loader shims stay disabled, reports `have_dynamic_loading=NO`, keeps direct
and prepared plugin install/uninstall SQL rejected before MariaDB execution,
compiles the dynamic-loader branches in `sql_plugin.cc` and fork-based
`dladdr()` stack symbol resolution out of the embedded profile, and leaves
static built-in plugin registration intact.

The event parse-data trim reduced the default archive by a further 5,536 bytes
with the same member count. The disabled profile now sets
`MYLITE_WITH_EVENT_PARSE_DATA_VALIDATION=OFF`, replaces
`event_parse_data.cc` with `mylite_event_parse_data_disabled.cc`, keeps direct
and prepared event DDL rejected before MariaDB execution, and preserves a
fail-closed raw embedded parser path without linking MariaDB's event schedule
validation body.

## Enabled Surface

The profile keeps the MariaDB components needed by the current embedded
bootstrap and storage-routing smoke coverage. The embedded archive includes the
static embedded server library and static embedded engines/plugins such as:

- Aria
- CSV
- HEAP/MEMORY
- InnoDB
- MyISAM and MRG_MyISAM
- partition support
- selected compatibility helpers such as type handlers and user variables

Some configured module metadata remains visible even when the
`libmariadbd.a` target does not build the module into the embedded archive.
Keep treating the configured surface as size-profile evidence: future profile
hardening should disable unwanted surfaces intentionally and record the effect.

## Disabled Or Missing Surface

The profile explicitly disables:

- WSREP/Galera
- Aria S3 support
- MariaDB upstream unit-test targets
- dynamic plugin loading and install/uninstall runtime
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
- MariaDB dynamic-column packed BLOB runtime and SQL functions
- top-level SQL `HANDLER` command runtime
- SQL sequence runtime and hidden SQL_SEQUENCE storage-engine wrapper
- virtual `SEQUENCE` storage engine
- SQL `HELP` command help-table implementation
- `PROCEDURE ANALYSE()` result-set analysis implementation
- generic SELECT procedure runtime
- view runtime and view `.frm` sidecar metadata helpers
- zlib-backed SQL `COMPRESS()` / `UNCOMPRESS()`, protocol compression, binlog
  compression, storage-engine-independent compressed columns, and retained
  InnoDB zlib page/compressed-page paths
- host dynamic-loader probes and `dlopen()` / `dlsym()` / `dladdr()` plugin
  loading paths
- stored routine, event, package, and stored-program instruction runtime
- event parse-data validation for event scheduler DDL
- trigger runtime and `.TRG` / `.TRN` sidecar metadata helpers
- dynamic UDF lookup, registration, and execution runtime
- binary-log transaction, event-write, and event-root core for the embedded
  no-binlog profile
- native MyISAM table maintenance, repair, key-cache assignment, and key
  preload administration
- `mysql.servers` foreign-server metadata cache
- external backup SQL runtime for `BACKUP STAGE` / `BACKUP LOCK`
- query cache runtime and administration
- statement profiling
- optimizer trace diagnostics
- static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES`
  information producers
- status metadata producers for `SHOW STATUS`,
  `INFORMATION_SCHEMA.GLOBAL_STATUS`, and
  `INFORMATION_SCHEMA.SESSION_STATUS` rows
- process-list metadata producers for `SHOW PROCESSLIST`,
  `SHOW FULL PROCESSLIST`, and `INFORMATION_SCHEMA.PROCESSLIST` rows
- routine metadata producers for `SHOW PROCEDURE STATUS`,
  `SHOW FUNCTION STATUS`, `INFORMATION_SCHEMA.ROUTINES`, and
  `INFORMATION_SCHEMA.PARAMETERS` rows
- C++ exception support in retained `sql_embedded` C++ compilation
- socket authentication
- feedback plugin
- Performance Schema
- thread-pool info plugin
- user-statistics plugin

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
| Archive size | 26,972,352 bytes / 25.72 MiB |
| Archive members | 673 |

The opt-in archive also replaces `event_parse_data.cc.o` with
`mylite_event_parse_data_disabled.cc.o`; its total size includes the current
static MyLite handler objects and should be compared only against matching
storage-smoke sources.

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
| MariaDB embedded archive | 26,776,960 bytes / 25.54 MiB | n/a | 670 | n/a |
| MariaDB storage-smoke archive | 26,972,352 bytes / 25.72 MiB | n/a | 673 | n/a |
| Embedded open-close smoke | 17,195,984 bytes / 16.40 MiB | 15,542,416 bytes / 14.82 MiB | n/a | 15,266 |
| Embedded exec smoke | 17,265,208 bytes / 16.47 MiB | 15,608,440 bytes / 14.89 MiB | n/a | 15,266 |
| Embedded statement smoke | 17,245,664 bytes / 16.45 MiB | 15,591,760 bytes / 14.87 MiB | n/a | 15,266 |
| Embedded warning smoke | 17,195,600 bytes / 16.40 MiB | 15,542,208 bytes / 14.82 MiB | n/a | 15,266 |
| Embedded comparison smoke | 17,302,352 bytes / 16.50 MiB | 15,593,008 bytes / 14.87 MiB | n/a | 15,268 |
| Storage-smoke open-close smoke | 17,332,704 bytes / 16.53 MiB | 15,626,000 bytes / 14.90 MiB | n/a | 15,266 |
| Storage-smoke exec smoke | 17,418,456 bytes / 16.61 MiB | 15,708,520 bytes / 14.98 MiB | n/a | 15,266 |
| Storage-smoke statement smoke | 17,382,400 bytes / 16.58 MiB | 15,675,344 bytes / 14.95 MiB | n/a | 15,266 |
| Storage-smoke warning smoke | 17,332,336 bytes / 16.53 MiB | 15,625,776 bytes / 14.90 MiB | n/a | 15,266 |
| Storage-smoke comparison smoke | 17,432,448 bytes / 16.62 MiB | 15,676,432 bytes / 14.95 MiB | n/a | 15,268 |
| Storage-engine smoke | 17,667,872 bytes / 16.85 MiB | 15,956,544 bytes / 15.22 MiB | n/a | 15,266 |

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
