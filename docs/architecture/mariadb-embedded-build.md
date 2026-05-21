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
size evidence. A build-local marker prevents no-op builds from repeatedly
stripping an already-stripped archive, keeping measurements stable. The wrapper
does not update MariaDB submodules.

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
MYLITE_WITH_QUERY_LOGS=OFF
MYLITE_WITH_SQL_DIGEST=OFF
MYLITE_WITH_STATUS_VARIABLES=OFF
MYLITE_WITH_STATEMENT_PROFILING_METADATA=OFF
MYLITE_WITH_ORACLE_COMPAT_FUNCTIONS=OFF
MYLITE_WITH_FULL_ERROR_MESSAGES=OFF
MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=OFF
MYLITE_WITH_PROCEDURE_ANALYSE=OFF
MYLITE_WITH_SYSVAR_HELP_TEXT=OFF
MYLITE_WITH_STATIC_SHOW_INFO=OFF
MYLITE_WITH_OPTION_HELP_TEXT=OFF
MYLITE_WITH_OPTIMIZER_TRACE=OFF
MYLITE_WITH_PROCESSLIST_METADATA=OFF
MYLITE_WITH_FOREIGN_SERVER_METADATA=OFF
MYLITE_WITH_BACKUP_RUNTIME=OFF
MYLITE_WITH_VIO_TLS=OFF
MYLITE_WITH_REPLICATION_EXEC_SYSVARS=OFF
MYLITE_WITH_REPLICATION_FILTERS=OFF
MYLITE_WITH_USERSTAT_DIAGNOSTICS=OFF
MYLITE_WITH_PROXY_PROTOCOL=OFF
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
disabled `@@have_profiling=NO` contract.
`MYLITE_WITH_STATEMENT_PROFILING_METADATA=OFF` replaces the remaining profiling
Information Schema metadata with a fail-closed stub. The embedded query cache
is compiled to no-op stubs and reports `@@have_query_cache=NO`. The embedded archive links
a small Oracle SQL-mode parser stub instead of the generated Oracle parser. It
also omits the fmtlib-backed `SFORMAT()` SQL function and builds the embedded
SQL target without C++ exceptions or unwind tables. Dynamic UDF shared-library
loading is omitted from the embedded archive. Dynamic plugin shared-object
loading and service injection are omitted behind
`MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=0`; static built-in plugins and native
storage engines still initialize, and the embedded profile reports
`@@have_dynamic_loading=NO`. The embedded baseline disables the active
binary-log transaction/event core behind `MYLITE_WITH_BINLOG_CORE=0` while
preserving the normal MariaDB server build path. It also guards embedded
no-binlog startup, open, cleanup, and GTID-index update paths, and omits the
unsupported injector root. It omits the general and slow query-log runtime
behind `MYLITE_WITH_QUERY_LOGS=0`; error logging,
SQL diagnostics, warnings, and result metadata remain available. It omits
statement digest normalization behind `MYLITE_WITH_SQL_DIGEST=0`; Performance
Schema digest text and hashes are unavailable, and startup sets
`@@max_digest_length=0` so per-session digest token buffers are not allocated.
SQL parsing, execution, prepared statements, diagnostics, and `EXPLAIN` remain
available. It omits full event parse-data validation behind
`MYLITE_WITH_EVENT_PARSE_DATA=0`; event DDL and metadata commands are rejected,
and retained parser references use a small link stub. It omits server
status-variable publication behind
`MYLITE_WITH_STATUS_VARIABLES=0`; `SHOW STATUS` and status Information Schema
tables return empty result sets, while ordinary SQL diagnostics and result
metadata remain available. It omits process-list metadata behind
`MYLITE_WITH_PROCESSLIST_METADATA=0`; `SHOW PROCESSLIST` is rejected, while
`INFORMATION_SCHEMA.PROCESSLIST` remains visible and returns zero rows. It
omits foreign-server metadata behind `MYLITE_WITH_FOREIGN_SERVER_METADATA=0`;
`CREATE SERVER`, `ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` are
rejected because they persist remote-server definitions in `mysql.servers`. It
omits the external backup runtime behind `MYLITE_WITH_BACKUP_RUNTIME=0`;
`BACKUP STAGE`, `BACKUP LOCK`, and `BACKUP UNLOCK` are rejected while ordinary
DDL backup hooks remain inert. It omits VIO TLS transport behind
`MYLITE_WITH_VIO_TLS=0` because the core embedded profile has no socket or
network handshake. It omits replication execution, slave protocol, replication
event, checksum, and semi-sync system variables behind
`MYLITE_WITH_REPLICATION_EXEC_SYSVARS=0`; compatibility variables such as
`@@log_bin=0` remain covered. It omits replication and binary-log filter
runtime behind `MYLITE_WITH_REPLICATION_FILTERS=0`; retained no-filter checks
remain permissive, and direct or prepared `@@replicate_do_db`,
`@@replicate_wild_ignore_table`, and `@@binlog_do_db` lookups fail as unknown
system variables. It omits PROXY protocol listener support behind
`MYLITE_WITH_PROXY_PROTOCOL=0`; the core embedded profile has no socket
listener, and direct or prepared `@@proxy_protocol_networks` lookups fail as an
unknown system variable. It omits user statistics diagnostics behind
`MYLITE_WITH_USERSTAT_DIAGNOSTICS=0`; `userstat` and
`INFORMATION_SCHEMA.CLIENT_STATISTICS`, `INDEX_STATISTICS`,
`TABLE_STATISTICS`, and `USER_STATISTICS` are server diagnostic counters, not
application storage behavior. It omits the `user_variables` Information Schema
plugin with `PLUGIN_USER_VARIABLES=NO`; local `@variable` SQL remains
available, but `INFORMATION_SCHEMA.USER_VARIABLES`, `SHOW USER_VARIABLES`, and
`FLUSH USER_VARIABLES` are omitted as diagnostics. It omits Oracle
compatibility function aliases and `oracle_schema` routing behind
`MYLITE_WITH_ORACLE_COMPAT_FUNCTIONS=0`, while
ordinary MySQL/MariaDB string functions remain available. It uses a compact
server error-message catalog behind `MYLITE_WITH_FULL_ERROR_MESSAGES=0`;
MariaDB error numbers and SQLSTATEs remain available, common syntax and
duplicate-key diagnostics stay readable, and uncommon inherited server errors
may use generic message text. It also
omits the legacy `PROCEDURE ANALYSE()` implementation behind
`MYLITE_WITH_PROCEDURE_ANALYSE=0`. Long system-variable help comments are
omitted behind `MYLITE_WITH_SYSVAR_HELP_TEXT=0`; variable names, values,
defaults, validation, and `SHOW VARIABLES` remain intact. Static `SHOW
AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` result producers are
omitted behind `MYLITE_WITH_STATIC_SHOW_INFO=0`. Command-line option help
prose in `my_long_options` is omitted behind
`MYLITE_WITH_OPTION_HELP_TEXT=0`; option names, parsing metadata, defaults,
and startup behavior remain intact. Optimizer trace diagnostics are omitted
behind `MYLITE_WITH_OPTIMIZER_TRACE=0`; ordinary planning, execution, and
`EXPLAIN` remain available.

## Measurement

Measured on 2026-05-21 from the imported MariaDB 11.8.6 source tree with the
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
| Archive size | 26,491,536 bytes / 25.26 MiB |
| Archive members | 702 |

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
bytes / 26.45 MiB. Omitting command-line option help text reduces the
pre-strip archive to 27,723,608 bytes / 26.44 MiB. Omitting optimizer trace
diagnostics reduces the pre-strip archive to 27,710,800 bytes / 26.43 MiB.
Omitting general and slow query-log runtime reduces the pre-strip archive to
27,689,312 bytes / 26.41 MiB. Omitting statement digest normalization reduces
the pre-strip archive to 27,627,712 bytes / 26.35 MiB. Omitting server
status-variable publication reduces the pre-strip archive to 27,591,584 bytes /
26.31 MiB. Omitting Oracle compatibility function aliases and `oracle_schema`
routing reduces the pre-strip archive to 27,446,520 bytes / 26.18 MiB.
Using the compact server error-message catalog reduces the pre-strip archive to
27,220,344 bytes / 25.96 MiB. Omitting dynamic plugin shared-object loading
and service injection reduces the pre-strip archive to 27,195,584 bytes /
25.94 MiB. Guarding embedded no-binlog startup, open, cleanup, and GTID-index
update paths, and omitting the unsupported injector root, reduces the pre-strip
archive to 27,180,312 bytes / 25.92 MiB. Omitting process-list metadata
producers reduces the pre-strip archive to 27,140,408 bytes / 25.88 MiB.
Omitting foreign-server metadata reduces the pre-strip archive to 27,124,416
bytes / 25.87 MiB. Omitting the external backup runtime reduces the pre-strip
archive to 27,118,776 bytes / 25.86 MiB. Omitting VIO TLS transport reduces the
pre-strip archive to 27,106,496 bytes / 25.85 MiB. Omitting replication
execution system variables reduces the pre-strip archive to 27,104,488 bytes /
25.85 MiB. Omitting PROXY protocol listener support reduces the pre-strip
archive to 27,097,424 bytes / 25.84 MiB. Omitting replication and binary-log
filter runtime reduces the pre-strip archive to 27,085,072 bytes /
25.83 MiB. Replacing statement profiling metadata with a fail-closed stub
reduces the pre-strip archive to 27,080,800 bytes / 25.83 MiB. Omitting user
statistics diagnostics reduces the pre-strip archive to 27,059,928 bytes /
25.81 MiB.
Post-build `strip -S -x` plus `ranlib` saves another 568,392 bytes
without changing archive membership or runtime behavior. The `SFORMAT()` and
exception cut accounts for 1,808,240
bytes, unwind-table omission saves another 10,840 bytes, and dynamic UDF
runtime omission saves 87,416 bytes and one archive member. The embedded
binary-log core trim saves 72,232 bytes and one archive member. Omitting
`PROCEDURE ANALYSE()` saves 39,120 bytes with no member-count change because
the implementation object is replaced by a small stub. Omitting
system-variable help text saves 56,040 bytes with no member-count change.
Omitting static `SHOW` information saves 32,936 bytes with no member-count
change. Omitting command-line option help text saves 8,680 bytes with no
member-count change. Omitting optimizer trace diagnostics saves 12,144 bytes
with no member-count change. Omitting general and slow query-log runtime saves
21,168 bytes with no member-count change. Omitting statement digest
normalization saves 56,480 bytes with no member-count change. Omitting server
status-variable publication saves 33,200 bytes with no member-count change.
Omitting Oracle compatibility function aliases and schema routing saves 145,064
bytes with no member-count change. Using the compact server error-message
catalog saves 226,176 bytes with no member-count change. Omitting dynamic
plugin shared-object loading and service injection saves 24,760 bytes with no
member-count change. Guarding the retained no-binlog paths and omitting the
injector root saves 14,896 bytes and one archive member. Omitting process-list
metadata producers saves 39,752 bytes with no member-count change. Omitting
foreign-server metadata saves 15,344 bytes with no member-count change.
Omitting the external backup runtime saves 5,520 bytes with no member-count
change. Omitting VIO TLS transport saves 12,296 bytes and one archive member,
and removes the linked `libssl` dependency from first-party embedded test
artifacts while keeping `libcrypto` for retained SQL crypto functions. Omitting
replication execution system variables saves 1,976 bytes with no member-count
change. Omitting PROXY protocol listener support saves 6,728 bytes with no
member-count change. Omitting replication and binary-log filter runtime saves
12,272 bytes with no member-count change. Replacing the remaining profiling
metadata saves 4,272 bytes with no member-count change. Omitting user
statistics diagnostics saves 20,872 bytes and one archive member.
The final archive is 5,038,168 bytes smaller than the Release build with
Performance Schema disabled, 6,638,104 bytes smaller than the symbol-stripped
baseline that still built Performance Schema, and 7,350,784 bytes smaller than
the original broad archive.

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
- selected static server plugins such as native type handlers

Configure also enables many module targets, including Archive, Blackhole,
CONNECT, Example, Federated, FederatedX, Mroonga, Sphinx, Spider, and many
server plugins. The `libmariadbd.a` target does not build every configured
module, but the enabled list is still important size-profile evidence because
future profile hardening should disable unwanted surfaces intentionally.
Performance Schema is not part of the default embedded archive; the
server-surface policy treats it as either omitted by the build profile or
disabled when a custom build includes it. `HELP` is present only as a small
unsupported-command shim in the embedded archive. Statement profiling reports
`@@have_profiling=NO`; top-level profiling commands and
`INFORMATION_SCHEMA.PROFILING` reads are rejected by policy, and the remaining
metadata source is a small unsupported-command shim in the embedded archive.
Query-cache implementation code is stubbed for the embedded archive; `SQL_CACHE`
and `SQL_NO_CACHE` remain accepted parser hints, while query-cache management
commands and variables are rejected by policy. Oracle SQL mode is rejected by
policy and linked to an unsupported parser stub; normal MariaDB/MySQL parsing
continues to use the generated MariaDB parser. Oracle compatibility aliases
such as `DECODE_ORACLE` and `LPAD_ORACLE`, plus `oracle_schema` routing, are
omitted from the embedded archive; normal `CONCAT`, `LPAD`, `RPAD`, `LTRIM`,
`RTRIM`, `SUBSTR`, `REPLACE`, `TRIM`, and `LENGTH` remain available.
`SFORMAT()` is omitted from the
embedded function registry, while ordinary `FORMAT()` remains available.
Dynamic UDF lookup, execution, and registration are omitted; stored functions
remain a separate SQL routine surface.
Dynamic plugin shared-object loading is omitted from the default embedded
archive; static built-in plugins and native storage engines remain available,
and the embedded profile reports `@@have_dynamic_loading=NO`.
The active binary-log transaction/event core is disabled in the default
embedded archive. The unsupported injector root is omitted, and retained
embedded no-binlog paths in `log.cc`, `mysqld.cc`, and transaction-coordinator
selection are guarded. `log_event.cc`, `log_event_server.cc`, `gtid_index.cc`,
`rpl_gtid.cc`, and shared helper symbols remain where generic MariaDB logging,
transaction coordination, or retained parser/runtime code still reference them.
Replication execution, slave protocol, replication-event, checksum, and
semi-sync system variables are omitted from the default embedded profile, while
compatibility variables such as `@@log_bin=0` remain covered.
Replication and binary-log filter runtime is omitted from the default embedded
profile; retained runtime checks behave as if no filters are configured, and
filter configuration variables are absent.
PROXY protocol listener support is omitted from the default embedded profile;
the core embedded runtime does not accept socket connections, and the
`proxy_protocol_networks` system variable is absent.
User statistics diagnostics are omitted from the default embedded profile; the
`userstat` system variable is absent, userstat Information Schema tables are
rejected by policy, and ordinary application tables with the same names remain
usable outside `information_schema`.
User-variable diagnostics are omitted from the default embedded profile; the
`user_variables` plugin is absent, diagnostic reads and resets are rejected by
policy, and ordinary `@variable` SQL remains available.
Unix socket server authentication is omitted from the default embedded profile;
the `unix_socket` plugin is absent, and network users and authentication remain
outside the core embedded API.
`PROCEDURE ANALYSE()` is omitted from the default embedded archive and linked
to an unsupported stub; ordinary SELECT execution and the generic retained
SELECT procedure dispatch continue to link. System-variable names, values,
defaults, validation, and `SHOW VARIABLES` remain available; only
`INFORMATION_SCHEMA.SYSTEM_VARIABLES.VARIABLE_COMMENT` is empty in the default
embedded profile. Static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and
`SHOW PRIVILEGES` are omitted from the default embedded archive and rejected by
policy; ordinary supported `SHOW` surfaces such as `SHOW VARIABLES` remain
available. Command-line option help prose is omitted from the default embedded
archive, while option names, aliases, argument types, defaults, limits, and
parsing behavior remain available. Optimizer trace diagnostics are omitted
from the default embedded archive and rejected by policy; ordinary query
planning, execution, and `EXPLAIN` remain available. General and slow query
logs are omitted from the default embedded archive and rejected by policy;
error logging, SQL diagnostics, warnings, and result metadata remain available.
Statement digest normalization is omitted from the default embedded archive
and `@@max_digest_length=0` is covered at startup. Performance Schema digest
text and hash diagnostics are unavailable, while SQL parsing, execution,
prepared statements, ordinary diagnostics, and `EXPLAIN` remain available.
Server status-variable publication is omitted from the default embedded archive;
`SHOW STATUS` and status Information Schema tables return empty result sets,
while SQL diagnostics, warnings, result metadata, and the public C API remain
available. Process-list metadata is omitted from the default embedded archive;
`SHOW PROCESSLIST` and `SHOW FULL PROCESSLIST` are rejected by policy, while
`INFORMATION_SCHEMA.PROCESSLIST` remains visible and returns zero rows.
Foreign-server metadata is omitted from the default embedded archive;
`CREATE SERVER`, `ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` are
rejected by policy because remote server definitions are server-global
`mysql.servers` metadata, not application tables inside the MyLite database
directory. External backup runtime SQL is omitted from the default embedded
archive; `BACKUP STAGE`, `BACKUP LOCK`, and `BACKUP UNLOCK` are rejected by
policy while ordinary DDL, transaction, and native storage behavior remain
available. The
full English server error-message catalog is compacted in the default embedded
archive; common syntax, duplicate-key, table lookup,
storage-engine, unsupported-feature, and unknown-function diagnostics remain
readable, and uncommon inherited server errors may report generic text while
the MariaDB errno and SQLSTATE remain available.

## Disabled Or Missing Surface

The baseline explicitly disables:

- WSREP/Galera
- Aria S3 support
- Performance Schema
- Feedback reporting
- SQL `HELP` table lookup
- Statement profiling
- Statement profiling metadata
- Query cache
- Oracle SQL mode
- `SFORMAT()`
- Dynamic UDF shared-library loading
- Dynamic plugin shared-object loading
- Active binary-log transaction/event core
- Unsupported binlog injector root
- `PROCEDURE ANALYSE()`
- System-variable help text
- Static `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES`
  information producers
- Command-line option help text
- Optimizer trace diagnostics
- General and slow query logs
- Statement digest diagnostics
- Server status variables
- Process-list metadata
- Foreign-server metadata
- External backup runtime
- VIO TLS transport
- Replication execution, slave protocol, and semi-sync system variables
- Replication and binary-log filter runtime
- PROXY protocol listener support
- User statistics diagnostics
- Oracle compatibility function aliases and `oracle_schema` routing
- Full inherited server error-message catalog
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
