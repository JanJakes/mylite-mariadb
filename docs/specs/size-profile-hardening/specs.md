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
The remaining statement-profiling metadata table is omitted for the same
reason.
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
application storage, SQL execution, or public API behavior. Statement digest
normalization is omitted because it feeds Performance Schema statement
diagnostics, which are already outside the default embedded profile. Server
status-variable publication is omitted because it exposes daemon diagnostic
counters rather than application SQL, storage, or public API behavior.
Process-list metadata is omitted because it exposes daemon thread and session
inventory rather than application SQL, storage, or public API behavior. Oracle
compatibility function aliases and `oracle_schema` routing are omitted because
Oracle compatibility mode is already unsupported and these aliases are not core
MySQL/MariaDB application SQL. The full English server error-message catalog is
compacted because uncommon diagnostic prose is not core SQL, storage, or API
behavior when MariaDB errno and SQLSTATE remain available.
VIO TLS transport is omitted because the core profile is an in-process
database-directory runtime without socket startup or network handshakes.
Replication execution system variables are omitted because they configure
server topology behavior that the embedded profile already rejects. PROXY
protocol listener support is omitted because the core profile has no socket
listener or network handshake. User statistics diagnostics are omitted because
they expose optional server counters rather than application SQL, native
storage, or public API behavior. User-variable diagnostics, Unix socket server
authentication, full event parse-data validation, and SQL `BINLOG` replay are
omitted because they belong to diagnostic, authentication, scheduler, and
replication replay surfaces outside the core embedded profile. Host-file
SELECT exports are omitted because they write arbitrary user-named filesystem
paths rather than returning rows through the embedded API. Startup option rows
for disabled binary-log, replication, and dynamic-plugin-loading surfaces are
omitted because the default `libmylite` startup contract is fixed,
serverless, and directory-owned. Inherited `#binlog_cache_files` setup is
skipped because the default profile has no binary-log core and does not create
binlog cache files. Row-replication type conversion is omitted because row-event
apply is already outside the core embedded profile. Binary-log event parser and
reader runtime is omitted because event decode and replay are already outside
the no-binlog embedded profile.

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
- Replacing the remaining disabled profiling metadata source with a
  fail-closed MyLite stub reduces the stripped archive to 26,511,064 bytes,
  25.28 MiB, and 703 members. The stripped `sql_profile.cc.o` member is
  replaced by a 2,656-byte `mylite_sql_profile_disabled.cc.o` member.
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
- Further guarding no-binlog startup, cleanup, open, annotated-row, and
  GTID-index update paths allows the unsupported `rpl_injector.cc.o` root to
  be omitted. The stripped archive is reduced to 26,609,024 bytes, 25.38 MiB,
  and 704 members. `gtid_index.cc.o`, `log_event.cc.o`, and
  `log_event_server.cc.o` still remain at this point because retained MariaDB
  code references event helpers.
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
- `mariadb/sql/sql_digest.cc` implements parser-token collection,
  statement normalization, digest text rendering, and digest MD5 calculation
  for Performance Schema statement diagnostics. Parser calls in
  `mariadb/sql/sql_lex.cc` only collect digest tokens when
  `mariadb/sql/sql_parse.cc` installs a Performance Schema digest listener.
  Disabling statement digest normalization behind
  `MYLITE_WITH_SQL_DIGEST=0`, replacing `sql_digest.cc.o` with
  `mylite_sql_digest_disabled.cc.o`, and starting with
  `--max-digest-length=0` reduces the stripped archive to 27,039,160 bytes,
  25.79 MiB, and 705 members.
- `mariadb/sql/mysqld.cc` defines `com_status_vars[]` and `status_vars[]`,
  then registers the status array during startup with `add_status_vars()`.
  `mariadb/sql/sql_show.cc` owns the dynamic `all_status_vars` registry and
  fills `SHOW STATUS`, `INFORMATION_SCHEMA.GLOBAL_STATUS`, and
  `INFORMATION_SCHEMA.SESSION_STATUS` from that registry.
- Disabling server status-variable publication behind
  `MYLITE_WITH_STATUS_VARIABLES=0` compiles the publication arrays to
  terminator-only arrays, makes the dynamic registry inert, and reduces the
  stripped archive to 27,005,960 bytes, 25.75 MiB, and 705 members.
- `mariadb/sql/sql_parse.cc` dispatches `COM_PROCESS_INFO` and
  `SQLCOM_SHOW_PROCESSLIST` to `mysqld_list_processes()`.
  `mariadb/sql/sql_show.cc` implements process-list row production by walking
  `server_threads` for `SHOW PROCESSLIST` and
  `INFORMATION_SCHEMA.PROCESSLIST`.
- Disabling process-list metadata behind
  `MYLITE_WITH_PROCESSLIST_METADATA=0` compiles out the daemon thread-walk
  producers, keeps `INFORMATION_SCHEMA.PROCESSLIST` visible with zero rows,
  and reduces the stripped archive to 26,569,272 bytes, 25.34 MiB, and 704
  members.
- `mariadb/sql/sql_servers.cc` initializes a process-global `servers_cache`,
  loads remote server definitions from `mysql.servers`, implements
  `CREATE SERVER`, `ALTER SERVER`, and `DROP SERVER`, and serves
  `SHOW CREATE SERVER` through `get_server_by_name()`.
- Disabling foreign-server metadata behind
  `MYLITE_WITH_FOREIGN_SERVER_METADATA=0` replaces that cache with a small
  unsupported embedded stub, rejects the SQL surface through policy, and
  reduces the stripped archive to 26,553,928 bytes, 25.32 MiB, and 704
  members.
- `mariadb/sql/backup.cc` implements `BACKUP STAGE`, `BACKUP LOCK`, backup
  metadata locks, and the `ddl.log` writer used by external backup tooling.
- Disabling the external backup runtime behind
  `MYLITE_WITH_BACKUP_RUNTIME=0` replaces the active runtime with inert DDL
  hooks and unsupported SQL entry points, and reduces the stripped archive to
  26,548,408 bytes, 25.32 MiB, and 704 members.
- `mariadb/vio/viossl.c` and `mariadb/vio/viosslfactories.c` implement TLS
  socket transport and connector/acceptor context setup. Shared VIO and
  client helpers in `mariadb/vio/vio.c`, `mariadb/vio/viosocket.c`, and
  `mariadb/sql-common/client.c` also reference those TLS helpers.
- Disabling VIO TLS transport behind `MYLITE_WITH_VIO_TLS=0` replaces the
  full transport with a small disabled stub, compiles shared VIO/client paths
  without TLS branches, removes `OpenSSL::SSL` from the first-party imported
  embedded target, and reduces the stripped archive to 26,536,112 bytes,
  25.31 MiB, and 703 members. `libcrypto` remains linked for retained SQL
  crypto and password functions.
- `mariadb/sql/sys_vars.cc` registers a contiguous block of replication
  execution, slave protocol, replication-event, checksum, and semi-sync system
  variables such as `slave_compressed_protocol`, `slave_type_conversions`, and
  `rpl_semi_sync_master_enabled`. Compatibility variables such as `log_bin`,
  `server_id`, and GTID positions are separate declarations.
- Disabling those registrations behind `MYLITE_WITH_REPLICATION_EXEC_SYSVARS=0`
  keeps retained shared replication helpers available for generic MariaDB code
  and reduces the stripped archive to 26,534,136 bytes, 25.30 MiB, and 703
  members.
- `mariadb/sql/proxy_protocol.cc` parses HAProxy PROXY protocol v1/v2 headers,
  parses `proxy_protocol_networks`, and checks whether a remote socket address
  may send a proxy header. `mariadb/sql/net_serv.cc` compiles
  `handle_proxy_header()` to `IGNORE` when `EMBEDDED_LIBRARY` is defined, and
  `mariadb/sql/sql_connect.cc` only calls `is_proxy_protocol_allowed()` in the
  non-embedded path. Shared `mysqld.cc` startup and cleanup still call the
  lifecycle helpers.
- Disabling PROXY protocol listener support behind
  `MYLITE_WITH_PROXY_PROTOCOL=0` replaces the parser/network-list object with a
  small disabled stub, omits the `proxy_protocol_networks` system variable, and
  reduces the stripped archive to 26,527,408 bytes, 25.30 MiB, and 703 members.
- `mariadb/sql/rpl_filter.cc` implements database, table, wildcard-table, and
  rewrite filters used by replication and binary logging. `mariadb/sql/mysqld.cc`
  registers corresponding `--replicate-*`, `--binlog-do-db`, and
  `--binlog-ignore-db` option rows, and `mariadb/sql/sys_vars.cc` exposes the
  matching filter system variables.
- Disabling replication and binary-log filter runtime behind
  `MYLITE_WITH_REPLICATION_FILTERS=0` replaces the filter parser with a
  permissive no-filter stub, omits inherited filter system variables and
  startup option rows, and reduces the stripped archive to 26,515,136 bytes,
  25.29 MiB, and 703 members.
- `mariadb/plugin/userstat/userstat.cc` declares four Information Schema
  plugins for `CLIENT_STATISTICS`, `INDEX_STATISTICS`, `TABLE_STATISTICS`, and
  `USER_STATISTICS`. `mariadb/sql/sys_vars.cc` exposes the `userstat` system
  variable that enables collection for those tables.
- Disabling user statistics diagnostics behind
  `MYLITE_WITH_USERSTAT_DIAGNOSTICS=0` omits the static `USERSTAT` plugin and
  the `userstat` system variable, and reduces the stripped archive to
  26,491,536 bytes, 25.26 MiB, and 702 members.
- `mariadb/plugin/user_variables/user_variables.cc` declares the optional
  `INFORMATION_SCHEMA.USER_VARIABLES` plugin plus the reset hook used by
  `FLUSH USER_VARIABLES`. Core `@variable` SQL behavior stays in retained
  SQL/session sources such as `item_func.cc`, `session_tracker.cc`, and
  `sql_class.cc`.
- Disabling user-variable diagnostics with `PLUGIN_USER_VARIABLES=NO` omits the
  static `user_variables` plugin, while retaining ordinary `@variable`
  assignment and reads. The stripped archive is reduced to 26,484,960 bytes,
  25.26 MiB, and 701 members.
- `mariadb/sql/sql_yacc.yy` still references `Event_parse_data::new_instance`
  while parsing event syntax, but `mariadb/sql/sql_parse.cc` returns an
  embedded-server unsupported error for event DDL when `HAVE_EVENT_SCHEDULER`
  is absent. The full `event_parse_data.cc` object validates scheduler dates
  and definitions for event execution that is not linked into `libmysqld`.
- Disabling full event parse-data validation behind
  `MYLITE_WITH_EVENT_PARSE_DATA=0` replaces `event_parse_data.cc` with a small
  parser-link stub. The stripped archive is reduced to 26,480,216 bytes,
  25.25 MiB, and 701 members.
- `mariadb/plugin/auth_socket/auth_socket.c` declares the `unix_socket`
  server authentication plugin for socket-client login. `libmylite` opens a
  database directory in-process and already rejects server account and password
  management statements.
- Disabling Unix socket server authentication with `PLUGIN_AUTH_SOCKET=NO`
  omits the static `auth_socket.c.o` object. The stripped archive is reduced
  to 26,478,056 bytes, 25.25 MiB, and 700 members.
- `mariadb/sql/sql_yacc.yy` parses SQL `BINLOG` replay statements as
  `SQLCOM_BINLOG_BASE64_EVENT`, while `mariadb/sql/sql_parse.cc` already
  returns a fail-closed embedded-server error for that command under
  `EMBEDDED_LIBRARY`.
- Disabling SQL `BINLOG` replay with `MYLITE_WITH_BINLOG_REPLAY=0` omits the
  embedded `sql_binlog.cc.o` object and rejects direct and prepared `BINLOG`
  statements through MyLite policy. The stripped archive is reduced to
  26,474,416 bytes, 25.25 MiB, and 699 members.
- `mariadb/sql/item_create.cc` registers Oracle compatibility aliases such as
  `DECODE_ORACLE`, `LPAD_ORACLE`, `RTRIM_ORACLE`, `SUBSTR_ORACLE`, and
  `CONCAT_OPERATOR_ORACLE`, and builds a separate
  `native_functions_hash_oracle` with Oracle-mode function overrides.
  `mariadb/sql/sql_schema.cc` routes `oracle_schema` and implied `MODE_ORACLE`
  lookup to that hash, while `mariadb/sql/sql_yacc.yy`, `item_func.*`,
  `item_cmpfunc.*`, `item_strfunc.*`, and `sql_lex.cc` still instantiate
  Oracle-only item classes.
- Disabling Oracle compatibility function aliases behind
  `MYLITE_WITH_ORACLE_COMPAT_FUNCTIONS=0` omits the aliases, the Oracle native
  function hash, `oracle_schema` routing, and Oracle-only item classes, while
  keeping normal `CONCAT`, `LPAD`, `RPAD`, `LTRIM`, `RTRIM`, `SUBSTR`,
  `REPLACE`, `TRIM`, and `LENGTH` behavior covered. The stripped archive is
  reduced to 26,861,088 bytes, 25.62 MiB, and 705 members.
- `mariadb/sql/derror.cc` initializes the English server error-message catalog
  by including generated `mysqld_ername.h`, then registers the populated error
  ranges for `my_error()` and `ER_DEFAULT()` lookups.
- Disabling the full English error-message catalog behind
  `MYLITE_WITH_FULL_ERROR_MESSAGES=0` keeps the same active error-id ranges
  and preserves common MyLite-facing messages such as syntax errors, duplicate
  keys, table lookup, storage-engine file errors, unsupported-feature
  diagnostics, and unknown function paths. Less common inherited server errors
  use a generic placeholder-free message while MariaDB errno and SQLSTATE
  remain available. The stripped archive is reduced to 26,647,312 bytes, 25.41
  MiB, and 705 members.
- `mariadb/sql/sql_plugin.cc` owns dynamic plugin shared-object loading,
  `--plugin-load`, `mysql.plugin` loading, `dlopen()` declaration discovery,
  and service pointer injection. `mariadb/sql/mysqld.cc` reports
  `@@have_dynamic_loading` from `HAVE_DLOPEN`.
- Disabling dynamic plugin loading behind
  `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=0` omits the runtime shared-object
  loader and dynamic service table while keeping static built-in plugins,
  native storage engines, and compression-provider fallback services available.
  The embedded profile reports `@@have_dynamic_loading=NO`. The stripped
  archive is reduced to 26,623,920 bytes, 25.39 MiB, and 705 members.
- `mariadb/sql/mysqld.cc` still owns `my_long_options[]` rows for disabled
  server topology and dynamic-plugin-loading startup options. The default
  `libmylite` startup vector uses retained serverless options such as
  `--skip-log-bin`, `--skip-slave-start`, `--plugin-dir`, and storage
  directory options, but does not use positive binlog, relay-log, replication,
  or plugin-load configuration rows.
- `mariadb/sql/log_cache.cc` implements inherited `#binlog_cache_files`
  directory setup and cleanup. The default embedded baseline already starts
  with `--skip-log-bin` and builds with `MYLITE_WITH_BINLOG_CORE=0`, so no
  retained runtime path needs binlog cache files.
- `mariadb/sql/rpl_utility_server.cc` implements row-replication type
  comparison, conversion-table construction, and binary-log type rendering for
  row-event apply. `mariadb/sql/rpl_utility.cc` still owns shared table-map
  metadata helpers used by retained event code, so the server-side conversion
  source needs a separate replacement rather than deleting all replication
  utility code.
- `mariadb/sql/log_event.cc` implements common binary-log event parser and
  reader runtime, including `Log_event::read_log_event()`,
  format-description setup, compressed-event helpers, and event-class virtual
  methods used by replay and replication paths. Retained code still references
  a small event-class link contract and `str_to_hex()` for ordinary SQL string
  literal rendering through `append_query_string()`.

## Proposed Design

After building the embedded archive, `tools/mariadb-embedded-build` strips
debug and local symbols from `libmariadbd.a` and refreshes the archive index
with `ranlib`. The wrapper records a build-local marker after stripping so a
no-op rebuild does not mutate an already-stripped archive or drift the measured
size.

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
custom MariaDB build enables the upstream profiling code. The default embedded
profile also replaces the remaining profiling Information Schema metadata with
a fail-closed stub, and MyLite rejects direct
`INFORMATION_SCHEMA.PROFILING` reads before dispatch.

The embedded query-cache implementation is compiled to no-op stubs. MyLite
keeps `SQL_CACHE` and `SQL_NO_CACHE` as accepted parser hints, reports
`@@have_query_cache=NO`, and rejects query-cache management commands and
variables before dispatch.

The embedded archive links a MyLite-owned Oracle parser stub instead of the
generated `yy_oracle.cc` parser. MyLite rejects attempts to enable
`sql_mode=ORACLE` before dispatch, while normal SQL modes and user variables
named `sql_mode` continue through the generated MariaDB parser.

The embedded archive omits Oracle compatibility function aliases and
`oracle_schema` routing by setting `MYLITE_WITH_ORACLE_COMPAT_FUNCTIONS=0` in
the MyLite baseline. The option defaults to `ON` so normal MariaDB server
builds keep upstream Oracle migration aliases. MyLite already rejects
`sql_mode=ORACLE`; this cut removes the remaining Oracle-only aliases and
parser item paths while keeping ordinary MySQL/MariaDB string functions
available.

The embedded archive uses a compact English server error-message catalog by
setting `MYLITE_WITH_FULL_ERROR_MESSAGES=0` in the MyLite baseline. The option
defaults to `ON` so normal MariaDB server builds keep upstream diagnostics.
The compact profile registers the same error ranges and keeps common
MyLite-facing format strings, but maps less common inherited server errors to
a generic placeholder-free message. MariaDB errno and SQLSTATE remain the
stable compatibility surfaces.

The embedded archive omits dynamic plugin shared-object loading by setting
`MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=0` in the MyLite baseline. The option
defaults to `ON` so normal MariaDB server builds keep upstream dynamic plugin
loading. The disabled profile keeps static built-in plugin registration and
initialization, keeps native storage engines available, and keeps provider
fallback services used by retained engine code. `INSTALL PLUGIN` and
`UNINSTALL PLUGIN` remain rejected by policy coverage, and
`@@have_dynamic_loading=NO` records the runtime contract.

The embedded archive omits startup option rows for disabled server topology and
dynamic-plugin-loading surfaces by setting
`MYLITE_WITH_DISABLED_STARTUP_OPTIONS=0` in the MyLite baseline. The option
defaults to `ON` so normal MariaDB server builds keep upstream option rows.
The disabled profile keeps startup options needed by the fixed `libmylite`
runtime, including `--skip-log-bin`, `--skip-slave-start`, `--plugin-dir`,
temporary-directory, and storage-directory options.

When `MYLITE_WITH_BINLOG_CORE=0`, the embedded archive keeps the
`init_binlog_cache_dir()` link contract but skips inherited
`#binlog_cache_files` setup and cleanup. Custom profiles that re-enable
binary logging keep upstream directory behavior.

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
transaction, row-event, event-write, and table-map entry points to
no-op or fail-closed behavior. The option defaults to `ON` so normal MariaDB
server builds keep upstream binlog behavior.
The disabled embedded profile also guards no-binlog startup, open, cleanup,
annotated-row, and GTID-index update paths, and omits the unsupported injector
root that is only needed by the server topology runtime.

The embedded archive omits SQL `BINLOG` statement replay by setting
`MYLITE_WITH_BINLOG_REPLAY=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream replay behavior. MyLite
policy rejects direct and prepared `BINLOG` statements before dispatch, while
the embedded MariaDB dispatcher remains a fail-closed backstop.

The embedded archive omits server-side binary-log event writers by setting
`MYLITE_WITH_LOG_EVENT_SERVER=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream event-writing behavior. The
disabled embedded source keeps the real `append_query_string()` SQL
string-quoting helper for ordinary `Item` and type rendering, while unsupported
binary-log event write paths fail closed.

The embedded archive omits replication GTID-state runtime by setting
`MYLITE_WITH_GTID_STATE=0` in the MyLite baseline. The option defaults to `ON`
so normal MariaDB server builds keep upstream GTID state behavior. The disabled
embedded source keeps empty state for retained no-binlog link paths and fails
closed on unsupported state mutation. MyLite policy rejects
`MASTER_GTID_WAIT()`, `BINLOG_GTID_POS()`,
`WSREP_SYNC_WAIT_UPTO_GTID()`, and GTID state variable assignments before
dispatch.

The embedded archive omits top-level SQL `HANDLER` command runtime by setting
`MYLITE_WITH_SQL_HANDLER=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream `HANDLER ...` behavior. The
disabled embedded source keeps generic cleanup hooks as no-ops and leaves the
storage-engine `handler` abstraction untouched. MyLite policy rejects direct
and prepared top-level `HANDLER ...` statements before dispatch.

The embedded archive omits `SELECT ... INTO OUTFILE` and
`SELECT ... INTO DUMPFILE` host-file writer bodies by setting
`MYLITE_WITH_SELECT_INTO_FILE=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream host-file export behavior.
The disabled embedded methods fail closed, and MyLite policy rejects direct
and prepared host-file SELECT exports before dispatch while retaining
`SELECT ... INTO @variable`.

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

The embedded archive omits statement digest normalization by setting
`MYLITE_WITH_SQL_DIGEST=0` in the MyLite baseline. The option defaults to `ON`
so normal MariaDB server builds keep upstream statement digest behavior. The
disabled profile links `mylite_sql_digest_disabled.cc` in place of
`sql_digest.cc`, preserving parser-visible helper symbols as no-ops while
returning empty digest text and a zero hash if a custom path asks for digest
output. MyLite starts with `--max-digest-length=0` so THD startup does not
allocate per-session digest token buffers. Performance Schema remains omitted
from the default profile.

The embedded archive omits server status-variable publication by setting
`MYLITE_WITH_STATUS_VARIABLES=0` in the MyLite baseline. The option defaults
to `ON` so normal MariaDB server builds keep upstream status counters. The
disabled profile compiles the status publication arrays to terminator-only
arrays, makes the dynamic registry helpers no-ops, and has `SHOW STATUS` plus
status Information Schema tables return empty result sets. Ordinary SQL
diagnostics, warnings, result metadata, and public C API diagnostics remain
available.

The embedded archive omits process-list metadata by setting
`MYLITE_WITH_PROCESSLIST_METADATA=0` in the MyLite baseline. The option
defaults to `ON` so normal MariaDB server builds keep upstream process-list
behavior. The disabled profile compiles out `SHOW PROCESSLIST` and
`INFORMATION_SCHEMA.PROCESSLIST` thread-walk producers, rejects
`SHOW PROCESSLIST` and `SHOW FULL PROCESSLIST` through policy, and keeps
`INFORMATION_SCHEMA.PROCESSLIST` queryable with zero rows.

The embedded archive omits foreign-server metadata by setting
`MYLITE_WITH_FOREIGN_SERVER_METADATA=0` in the MyLite baseline. The option
defaults to `ON` so normal MariaDB server builds keep upstream `mysql.servers`
behavior. The disabled profile replaces `sql_servers.cc` with a small stub
that starts without loading `mysql.servers`, rejects `CREATE SERVER`,
`ALTER SERVER`, and `DROP SERVER` if policy is bypassed, and makes
`SHOW CREATE SERVER` report the inherited not-found path. The public MyLite
SQL policy rejects direct and prepared foreign-server metadata statements.

The embedded archive omits the external backup runtime by setting
`MYLITE_WITH_BACKUP_RUNTIME=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB server builds keep upstream backup behavior. The
disabled profile replaces `backup.cc` with inert DDL helper hooks and
unsupported `BACKUP STAGE` / `BACKUP LOCK` entry points. The public MyLite SQL
policy rejects direct and prepared backup statements before MariaDB dispatch.

The embedded archive omits VIO TLS transport by setting
`MYLITE_WITH_VIO_TLS=0` in the MyLite baseline. The option defaults to `ON` so
normal MariaDB builds keep upstream TLS socket behavior. The disabled profile
replaces `viossl.c` and `viosslfactories.c` with a small unsupported transport
stub, guards TLS branches in shared VIO/client helpers, makes inherited
`mysql_ssl_set()` calls fail closed, and lets first-party linked MyLite
artifacts omit `OpenSSL::SSL`.

The embedded archive omits replication execution system-variable registration
by setting `MYLITE_WITH_REPLICATION_EXEC_SYSVARS=0` in the MyLite baseline. The
option defaults to `ON` so normal MariaDB builds keep upstream variable
registration. The disabled profile removes variable rows for the guarded
replication execution, slave protocol, checksum, replication-event, and
semi-sync variables while keeping compatibility variables such as `@@log_bin=0`
available.

The embedded archive omits replication and binary-log filter runtime by setting
`MYLITE_WITH_REPLICATION_FILTERS=0` in the MyLite baseline. The option defaults
to `ON` so normal MariaDB builds keep upstream filtering behavior. The disabled
profile replaces `rpl_filter.cc` with a permissive no-filter stub and omits
filter configuration variables such as `replicate_do_db`,
`replicate_wild_ignore_table`, and `binlog_do_db`.

The embedded archive omits row-replication type conversion by setting
`MYLITE_WITH_RPL_TYPE_CONVERSION=0` in the MyLite baseline. The option defaults
to `ON` so upstream-style embedded builds keep row-event conversion behavior.
The disabled profile replaces `rpl_utility_server.cc` with fail-closed
conversion stubs while retaining `rpl_utility.cc` table-map metadata helpers.

The embedded archive omits common binary-log event parser and reader runtime by
setting `MYLITE_WITH_LOG_EVENT_PARSING=0` in the MyLite baseline. The option
defaults to `ON` so upstream-style embedded builds keep upstream event parsing
behavior. The disabled profile omits `log_event.cc` and keeps minimal
fail-closed event reader, checksum metadata, virtual method, destructor, and
format-description symbols in `mylite_log_event_server_disabled.cc`.
Configuration rejects `MYLITE_WITH_LOG_EVENT_PARSING=0` while upstream server
event writers remain enabled, because the disabled event source owns the
combined retained link contract.
`append_query_string()` and `str_to_hex()` remain available for ordinary SQL
literal rendering.

The embedded archive omits binary-log GTID-index runtime by setting
`MYLITE_WITH_GTID_INDEX=0` in the MyLite baseline. The option defaults to `ON`
so upstream-style embedded builds keep upstream binlog GTID-index behavior.
The disabled profile replaces `gtid_index.cc` with a fail-closed embedded
source, and binary-log GTID-index tuning system variables are omitted from the
default profile.

The embedded archive omits PROXY protocol listener support by setting
`MYLITE_WITH_PROXY_PROTOCOL=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB builds keep upstream listener behavior. The disabled
profile replaces `proxy_protocol.cc` with fail-closed lifecycle stubs and omits
the `proxy_protocol_networks` system-variable registration.

The embedded archive omits user statistics diagnostics by setting
`MYLITE_WITH_USERSTAT_DIAGNOSTICS=0` in the MyLite baseline. The option
defaults to `ON` so normal MariaDB builds keep upstream userstat diagnostics.
The disabled profile omits the static `USERSTAT` plugin and the `userstat`
system-variable registration. MyLite policy rejects direct and prepared reads
from the userstat Information Schema tables, `FLUSH *_STATISTICS`, and
`userstat` system-variable assignment, while ordinary application tables with
the same names remain valid outside `information_schema`.

The embedded archive omits user-variable diagnostics by setting
`PLUGIN_USER_VARIABLES=NO` in the MyLite baseline. The disabled profile omits
the optional `user_variables` Information Schema plugin. MyLite policy rejects
direct and prepared reads from `INFORMATION_SCHEMA.USER_VARIABLES`, `SHOW
USER_VARIABLES`, and `FLUSH USER_VARIABLES`, while ordinary `@variable` SQL
and application tables named `user_variables` remain valid.

The embedded archive omits Unix socket server authentication by setting
`PLUGIN_AUTH_SOCKET=NO` in the MyLite baseline. The disabled profile omits the
static `unix_socket` authentication plugin. This does not change `libmylite`
opens because the core API does not perform socket-client authentication, and
server account management is already rejected by policy.

The embedded archive omits full event parse-data validation by setting
`MYLITE_WITH_EVENT_PARSE_DATA=0` in the MyLite baseline. The option defaults to
`ON` so normal MariaDB builds keep upstream event validation. The disabled
profile retains only enough `Event_parse_data` implementation for parser-owned
event syntax to link, while MyLite policy rejects direct and prepared event
DDL and event metadata statements before parser execution.

Archive stripping stays enabled by default because it is the distributed archive
profile. Developers can set `STRIP_ARCHIVE=0` when they
need an unstripped archive for local inspection.

## Affected MariaDB Subsystems

The Performance Schema storage-engine plugin and Feedback reporting plugin are
omitted by CMake configuration, embedded `HELP`, statement profiling, query
cache, Oracle SQL mode, `SFORMAT()`, and `PROCEDURE ANALYSE()` are compiled to
disabled or omitted surfaces, and the compiled objects use size-oriented
release flags. The embedded SQL target also uses `-fno-exceptions`, omits
unwind tables, and omits dynamic UDF lookup, execution, and DDL runtime. The
embedded baseline also disables binlog transaction/event runtime behind a
MyLite-owned profile flag, guards retained no-binlog paths, and omits the
unsupported injector root from the no-binlog embedded profile. It also omits
long system-variable help comments from `sys_vars.cc`. Static server-information
`SHOW` producers are compiled out of
`sql_show.cc`. Hardcoded command-line option help strings are compiled out of
the embedded `mysqld.cc` option table. Optimizer trace diagnostics are replaced
with inert trace helper symbols in the embedded SQL archive. General and slow
query-log handlers are compiled to inert embedded paths while shared error-log
behavior remains available. Statement digest normalization is replaced with
inert digest helper symbols, and per-session digest token buffers are disabled
at startup. Server status-variable publication arrays and dynamic registry
updates are compiled to empty embedded paths. Process-list metadata row
producers are compiled out while the information-schema table shape remains
visible. The embedded English server error-message catalog is compacted while
keeping MariaDB error numbers,
SQLSTATEs, and common diagnostics available. Dynamic plugin shared-object
loading and the corresponding dynamic service injection table are compiled out
of the default embedded archive, while static built-in plugins, native engines,
and provider fallback services remain available. VIO TLS transport is replaced
with a disabled embedded stub. Replication execution, slave protocol,
replication-event, checksum, and semi-sync system-variable registrations are
compiled out while retained shared replication helpers remain available. PROXY
protocol listener parsing and its system-variable registration are replaced
with fail-closed embedded stubs. User statistics Information Schema plugin
registration and the `userstat` system variable are omitted. User-variable
diagnostics, Unix socket server authentication, full event parse-data
validation, and SQL `BINLOG` replay are also omitted from the default embedded
profile. Server-side binary-log event writers are replaced with a disabled
embedded source while the ordinary SQL string-quoting helper remains available.
Row-replication type-conversion helpers are replaced with fail-closed embedded
stubs while shared table-map metadata helpers remain available for retained
event code. Common binary-log event parser and reader runtime is omitted while
minimal fail-closed event-class symbols remain for retained unsupported paths.

## Compatibility Impact

No application compatibility impact is expected. This slice does not remove SQL
syntax, functions, data types, collations, supported storage engines,
ordinary diagnostics, or public C API behavior. Performance Schema remains
outside the core embedded profile, and Feedback reporting is not part of the
embedded runtime contract. SQL `HELP` is a server help-table surface and is
explicitly unsupported in the embedded profile. Statement profiling is a diagnostic
server surface, not application data behavior, and is explicitly unsupported in
the embedded profile, including its `INFORMATION_SCHEMA.PROFILING` metadata
table. Query-cache management is a server-side result-cache
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
engine coverage intact. Omitting the injector root does not affect supported
SQL because it is only used by the unsupported server topology runtime.
SQL `BINLOG` replay is also a replication replay surface. Omitting
`sql_binlog.cc` and rejecting direct and prepared `BINLOG` statements does not
affect ordinary SQL execution, prepared statements, native storage engines,
JSON, GEOMETRY/GIS, DDL, DML, transactions, or the public C API.
Common binary-log event parser and reader runtime is also part of the
unsupported replication/binlog replay surface. Omitting `log_event.cc` does not
affect ordinary SQL parsing, statement execution, prepared statements,
diagnostics, native storage engines, JSON, GEOMETRY/GIS, DDL, DML,
transactions, or the public C API; retained event symbols fail closed if an
unsupported path reaches them.
Omitting the guarded replication execution system variables only removes
configuration rows for unsupported topology behavior; common compatibility
variables such as `@@log_bin=0` remain available.
Omitting replication and binary-log filter runtime only removes inherited
topology-filter configuration. Retained no-filter checks allow ordinary SQL to
continue unchanged because the embedded profile has no configured replication
or binlog filters.
Omitting row-replication type conversion only removes conversion decisions for
unsupported row-event apply. Ordinary SQL type conversion, native storage,
JSON, GEOMETRY/GIS, sequence handling, prepared statements, and the public C API
continue through retained non-replication paths.
Omitting PROXY protocol listener support only removes inherited socket-listener
configuration and header parsing. The core embedded profile already starts with
`--skip-networking` and does not accept socket connections.
Omitting user statistics diagnostics only removes optional server counter
tables and the `userstat` collection knob. It does not affect ordinary SQL,
native storage, JSON, GEOMETRY/GIS, diagnostics, result metadata, prepared
statements, or the public C API.
Omitting user-variable diagnostics only removes the optional
`INFORMATION_SCHEMA.USER_VARIABLES` plugin and reset command. Ordinary
`@variable` SQL behavior remains covered. Omitting Unix socket server
authentication only removes a socket-client login plugin; `libmylite` opens a
local database directory directly. Omitting full event parse-data validation
only removes scheduler validation code for event execution that the embedded
profile already rejects.
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
Statement digest normalization is a Performance Schema diagnostic path.
Omitting it removes normalized statement digest text and digest hashes, not
ordinary parsing, statement execution, prepared statements, `EXPLAIN`, SQL
diagnostics, JSON, GEOMETRY/GIS, native storage engines, DDL, DML, or the
public C API.
Server status-variable publication is a server diagnostic path. Omitting it
removes `SHOW STATUS` rows and status Information Schema rows, not ordinary
SQL diagnostics, warnings, result metadata, prepared statements, JSON,
GEOMETRY/GIS, native storage engines, DDL, DML, or the public C API.
Process-list metadata is a daemon thread/session inventory surface. Omitting it
rejects `SHOW PROCESSLIST` and makes `INFORMATION_SCHEMA.PROCESSLIST` return
zero rows; it does not affect ordinary SQL diagnostics, warnings, result
metadata, prepared statements, JSON, GEOMETRY/GIS, native storage engines, DDL,
DML, or the public C API.
The full English server error-message catalog is diagnostic text. Compacting it
does not change MariaDB errno, SQLSTATE, SQL execution, prepared statements,
warnings, result metadata, JSON, GEOMETRY/GIS, native storage engines, DDL,
DML, or the public C API. Common syntax-error and duplicate-key text remains
covered; less common inherited server errors may return generic text.
Dynamic plugin loading is already outside the embedded core API. Omitting the
shared-object loader does not affect static built-in plugins, native storage
engines, SQL parsing, DDL, DML, prepared statements, diagnostics, result
metadata, JSON, or GEOMETRY/GIS.

## Database-Directory And Lifecycle Impact

Runtime directory layout, storage files, temporary files, and lock behavior are
unchanged. Query-log omission removes inherited daemon log-file output rather
than adding any database-directory companions. Statement digest trimming only
removes in-memory diagnostic collection and token buffers. Status-variable
trimming only removes in-memory diagnostic publication. Dynamic plugin loading
trimming does not remove the transient database-local plugin directory because
retained MariaDB startup code still expects the directory setting.

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
Omitting statement digest normalization brings the current archive to
27,039,160 bytes / 25.79 MiB, 4,490,544 bytes smaller than the Release build
with Performance Schema disabled, 6,090,480 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 6,803,160
bytes smaller than the original broad archive.
Omitting server status-variable publication brings the current archive to
27,005,960 bytes / 25.75 MiB, 4,523,744 bytes smaller than the Release build
with Performance Schema disabled, 6,123,680 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 6,836,360
bytes smaller than the original broad archive.
Omitting Oracle compatibility function aliases and `oracle_schema` routing
brings the current archive to 26,861,088 bytes / 25.62 MiB, 4,668,616 bytes
smaller than the Release build with Performance Schema disabled, 6,268,552
bytes smaller than the symbol-stripped baseline with Performance Schema still
built, and 6,981,232 bytes smaller than the original broad archive.
Using the compact server error-message catalog brings the current archive to
26,647,312 bytes / 25.41 MiB, 4,882,392 bytes smaller than the Release build
with Performance Schema disabled, 6,482,328 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,195,008
bytes smaller than the original broad archive.
Omitting dynamic plugin shared-object loading and service injection brings the
current archive to 26,623,920 bytes / 25.39 MiB, 4,905,784 bytes smaller than
the Release build with Performance Schema disabled, 6,505,720 bytes smaller
than the symbol-stripped baseline with Performance Schema still built, and
7,218,400 bytes smaller than the original broad archive.
Guarding retained no-binlog paths and omitting the injector root brings the
current archive to 26,609,024 bytes / 25.38 MiB, 4,920,680 bytes smaller than
the Release build with Performance Schema disabled, 6,520,616 bytes smaller
than the symbol-stripped baseline with Performance Schema still built, and
7,233,296 bytes smaller than the original broad archive.
Omitting the external backup runtime brings the current archive to 26,548,408
bytes / 25.32 MiB, 4,981,296 bytes smaller than the Release build with
Performance Schema disabled, 6,581,232 bytes smaller than the symbol-stripped
baseline with Performance Schema still built, and 7,293,912 bytes smaller than
the original broad archive.
Omitting VIO TLS transport brings the current archive to 26,536,112 bytes /
25.31 MiB, 4,993,592 bytes smaller than the Release build with Performance
Schema disabled, 6,593,528 bytes smaller than the symbol-stripped baseline
with Performance Schema still built, and 7,306,208 bytes smaller than the
original broad archive.
Omitting replication execution system variables brings the current archive to
26,534,136 bytes / 25.30 MiB, 4,995,568 bytes smaller than the Release build
with Performance Schema disabled, 6,595,504 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,308,184
bytes smaller than the original broad archive.
Omitting PROXY protocol listener support brings the current archive to
26,527,408 bytes / 25.30 MiB, 5,002,296 bytes smaller than the Release build
with Performance Schema disabled, 6,602,232 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,314,912
bytes smaller than the original broad archive.
Omitting replication and binary-log filter runtime brings the current archive
to 26,515,136 bytes / 25.29 MiB, 5,014,568 bytes smaller than the Release build
with Performance Schema disabled, 6,614,504 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,327,184
bytes smaller than the original broad archive.
Replacing the remaining profiling metadata with a fail-closed stub brings the
current archive to 26,511,064 bytes / 25.28 MiB, 5,018,640 bytes smaller than
the Release build with Performance Schema disabled, 6,618,576 bytes smaller
than the symbol-stripped baseline with Performance Schema still built, and
7,331,256 bytes smaller than the original broad archive.
Omitting user statistics diagnostics brings the current archive to
26,491,536 bytes / 25.26 MiB, 5,038,168 bytes smaller than the Release build
with Performance Schema disabled, 6,638,104 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,350,784
bytes smaller than the original broad archive.
Omitting user-variable diagnostics brings the current archive to
26,484,960 bytes / 25.26 MiB, 5,044,744 bytes smaller than the Release build
with Performance Schema disabled, 6,644,680 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,357,360
bytes smaller than the original broad archive.
Reducing event parse-data validation to a parser-link stub brings the current
archive to 26,480,216 bytes / 25.25 MiB, 5,049,488 bytes smaller than the
Release build with Performance Schema disabled, 6,649,424 bytes smaller than
the symbol-stripped baseline with Performance Schema still built, and
7,362,104 bytes smaller than the original broad archive.
Omitting Unix socket server authentication brings the current archive to
26,478,056 bytes / 25.25 MiB, 5,051,648 bytes smaller than the Release build
with Performance Schema disabled, 6,651,584 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,364,264
bytes smaller than the original broad archive.
Omitting SQL `BINLOG` statement replay brings the current archive to
26,474,416 bytes / 25.25 MiB, 5,055,288 bytes smaller than the Release build
with Performance Schema disabled, 6,655,224 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,367,904
bytes smaller than the original broad archive.
Omitting persistent optimizer-statistics storage and JSON histogram storage
brings the current archive to 26,402,232 bytes / 25.18 MiB, 5,127,472 bytes
smaller than the Release build with Performance Schema disabled, 6,727,408
bytes smaller than the symbol-stripped baseline with Performance Schema still
built, and 7,440,088 bytes smaller than the original broad archive.
Omitting server-side binary-log event writers brings the current archive to
26,341,584 bytes / 25.12 MiB, 5,188,120 bytes smaller than the Release build
with Performance Schema disabled, 6,788,056 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,500,736
bytes smaller than the original broad archive.
Omitting replication GTID-state runtime brings the current archive to
26,287,896 bytes / 25.07 MiB, 5,241,808 bytes smaller than the Release build
with Performance Schema disabled, 6,841,744 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,554,424
bytes smaller than the original broad archive.
Omitting SQL `HANDLER` command runtime brings the current archive to
26,275,784 bytes / 25.06 MiB, 5,253,920 bytes smaller than the Release build
with Performance Schema disabled, 6,853,856 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,566,536
bytes smaller than the original broad archive.
Omitting `SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` host-file
writers brings the current archive to 26,269,664 bytes / 25.05 MiB, 5,260,040
bytes smaller than the Release build with Performance Schema disabled,
6,859,976 bytes smaller than the symbol-stripped baseline with Performance
Schema still built, and 7,572,656 bytes smaller than the original broad
archive.
Omitting startup option rows for disabled server topology and dynamic
plugin-loading surfaces brings the current archive to 26,267,304 bytes /
25.05 MiB, 5,262,400 bytes smaller than the Release build with Performance
Schema disabled, 6,862,336 bytes smaller than the symbol-stripped baseline
with Performance Schema still built, and 7,575,016 bytes smaller than the
original broad archive.
Skipping inherited `#binlog_cache_files` setup in the no-binlog embedded
profile brings the current archive to 26,265,424 bytes / 25.05 MiB, 5,264,280
bytes smaller than the Release build with Performance Schema disabled,
6,864,216 bytes smaller than the symbol-stripped baseline with Performance
Schema still built, and 7,576,896 bytes smaller than the original broad
archive.
Replacing row-replication type conversion with fail-closed embedded stubs
brings the current archive to 26,258,720 bytes / 25.04 MiB, 5,270,984 bytes
smaller than the Release build with Performance Schema disabled, 6,870,920
bytes smaller than the symbol-stripped baseline with Performance Schema still
built, and 7,583,600 bytes smaller than the original broad archive.
Omitting binary-log event parser and reader runtime brings the current archive
to 26,195,576 bytes / 24.98 MiB, 5,334,128 bytes smaller than the Release build
with Performance Schema disabled, 6,934,064 bytes smaller than the
symbol-stripped baseline with Performance Schema still built, and 7,646,744
bytes smaller than the original broad archive.
Omitting binary-log GTID-index runtime and tuning variables brings the current
archive to 26,180,192 bytes / 24.97 MiB, 5,349,512 bytes smaller than the
Release build with Performance Schema disabled, 6,949,448 bytes smaller than
the symbol-stripped baseline with Performance Schema still built, and
7,662,128 bytes smaller than the original broad archive.

## License Or Dependency Impact

No new dependencies or license changes. The wrapper uses standard `strip` and
`ranlib` tools already expected in the native build toolchain. VIO TLS trimming
removes the linked `libssl` dependency from first-party embedded MyLite
artifacts while retaining `libcrypto` for SQL crypto and password functions.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `sql_analyse.cc.o` is absent and
  `mylite_procedure_analyse_stub.cc.o` is present in `libmariadbd.a`.
- Confirm `opt_trace.cc.o` is absent and `mylite_opt_trace_disabled.cc.o` is
  present in `libmariadbd.a`.
- Confirm `MYLITE_WITH_QUERY_LOGS=OFF` appears in the embedded CMake cache and
  query-log configuration SQL is rejected by server-surface policy coverage.
- Confirm `MYLITE_WITH_SQL_DIGEST=OFF` appears in the embedded CMake cache,
  `sql_digest.cc.o` is absent, `mylite_sql_digest_disabled.cc.o` is present in
  `libmariadbd.a`, and `@@max_digest_length=0` is covered by server-surface
  policy coverage.
- Confirm `MYLITE_WITH_STATUS_VARIABLES=OFF` appears in the embedded CMake
  cache, and direct and prepared `SHOW STATUS` plus status Information Schema
  reads return empty rows through server-surface policy coverage.
- Confirm `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=OFF` appears in the embedded
  CMake cache, `@@have_dynamic_loading=NO` is covered by server-surface policy
  coverage, and plugin SQL remains rejected.
- Confirm `MYLITE_WITH_LOG_EVENT_SERVER=OFF` appears in the embedded CMake
  cache.
- Confirm `MYLITE_WITH_LOG_EVENT_PARSING=OFF` appears in the embedded CMake
  cache.
- Confirm `MYLITE_WITH_GTID_STATE=OFF` appears in the embedded CMake cache.
- Confirm `MYLITE_WITH_GTID_INDEX=OFF` appears in the embedded CMake cache.
- Confirm `MYLITE_WITH_SQL_HANDLER=OFF` appears in the embedded CMake cache.
- Confirm `MYLITE_WITH_SELECT_INTO_FILE=OFF` appears in the embedded CMake
  cache.
- Confirm `rpl_injector.cc.o`, `rpl_record.cc.o`, `log_event.cc.o`,
  `log_event_server.cc.o`, and `gtid_index.cc.o` are absent from
  `libmariadbd.a`, while `mylite_gtid_index_disabled.cc.o`,
  `mylite_log_event_server_disabled.cc.o`, and
  `mylite_rpl_gtid_disabled.cc.o` remain.
- Confirm `rpl_gtid.cc.o` is absent from `libmariadbd.a`, and direct and
  prepared `MASTER_GTID_WAIT()` / `BINLOG_GTID_POS()` /
  `WSREP_SYNC_WAIT_UPTO_GTID()` calls plus GTID state variable assignments fail
  through server-surface policy coverage.
- Confirm `sql_handler.cc.o` is absent from `libmariadbd.a`,
  `mylite_sql_handler_disabled.cc.o` remains, and direct and prepared
  top-level `HANDLER ...` statements fail through server-surface policy
  coverage.
- Confirm direct and prepared `SELECT ... INTO OUTFILE` and
  `SELECT ... INTO DUMPFILE` statements fail through server-surface policy
  coverage while `SELECT ... INTO @variable` succeeds.
- Confirm `MYLITE_WITH_BINLOG_REPLAY=OFF` appears in the embedded CMake cache,
  `sql_binlog.cc.o` is absent from `libmariadbd.a`, and direct and prepared
  SQL `BINLOG` replay statements fail through server-surface policy coverage.
- Confirm `MYLITE_WITH_PROCESSLIST_METADATA=OFF` appears in the embedded CMake
  cache, direct and prepared process-list SHOW commands are rejected, and
  `INFORMATION_SCHEMA.PROCESSLIST` returns zero rows.
- Confirm `MYLITE_WITH_FOREIGN_SERVER_METADATA=OFF` appears in the embedded
  CMake cache, `sql_servers.cc.o` is absent,
  `mylite_sql_servers_disabled.cc.o` is present in `libmariadbd.a`, and direct
  and prepared foreign-server metadata statements are rejected.
- Confirm `MYLITE_WITH_BACKUP_RUNTIME=OFF` appears in the embedded CMake cache,
  `backup.cc.o` is absent, `mylite_backup_disabled.cc.o` is present in
  `libmariadbd.a`, and direct and prepared backup SQL statements are rejected.
- Confirm `MYLITE_WITH_VIO_TLS=OFF` appears in the embedded CMake cache,
  `viossl.c.o` and `viosslfactories.c.o` are absent,
  `mylite_viossl_disabled.c.o` is present in `libmariadbd.a`, and
  first-party linked embedded artifacts do not depend on `libssl`.
- Confirm `MYLITE_WITH_REPLICATION_EXEC_SYSVARS=OFF` appears in the embedded
  CMake cache, direct and prepared `@@slave_type_conversions` lookups fail with
  unknown-system-variable errno, and `SHOW VARIABLES` does not expose
  `slave_type_conversions` or `rpl_semi_sync_master_enabled`.
- Confirm `MYLITE_WITH_PROXY_PROTOCOL=OFF` appears in the embedded CMake cache,
  `proxy_protocol.cc.o` is absent, `mylite_proxy_protocol_disabled.cc.o` is
  present in `libmariadbd.a`, and direct and prepared
  `@@proxy_protocol_networks` lookups fail with unknown-system-variable errno.
- Confirm `MYLITE_WITH_REPLICATION_FILTERS=OFF` appears in the embedded CMake
  cache, `rpl_filter.cc.o` is absent, `mylite_rpl_filter_disabled.cc.o` is
  present in `libmariadbd.a`, and direct and prepared
  `@@replicate_do_db`, `@@replicate_wild_ignore_table`, and `@@binlog_do_db`
  lookups fail with unknown-system-variable errno.
- Confirm `MYLITE_WITH_USERSTAT_DIAGNOSTICS=OFF` appears in the embedded CMake
  cache, `userstat.cc.o` is absent from `libmariadbd.a`, `@@userstat` lookups
  fail with unknown-system-variable errno, and userstat diagnostic SQL is
  rejected by server-surface policy coverage.
- Confirm `PLUGIN_USER_VARIABLES=NO` appears in the embedded CMake cache,
  `user_variables.cc.o` is absent from `libmariadbd.a`, ordinary `@variable`
  SQL still works, and user-variable diagnostic SQL is rejected by
  server-surface policy coverage.
- Confirm `PLUGIN_AUTH_SOCKET=NO` appears in the embedded CMake cache,
  `auth_socket.c.o` is absent from `libmariadbd.a`, and the `unix_socket`
  plugin is absent from Information Schema plugin metadata.
- Confirm `MYLITE_WITH_EVENT_PARSE_DATA=OFF` appears in the embedded CMake
  cache, `event_parse_data.cc.o` is absent,
  `mylite_event_parse_data_disabled.cc.o` is present in `libmariadbd.a`, and
  direct and prepared event DDL plus event metadata SQL are rejected by
  server-surface policy coverage.
- Confirm `MYLITE_WITH_PERSISTENT_STATISTICS=OFF` appears in the embedded
  CMake cache, `sql_statistics.cc.o` and `opt_histogram_json.cc.o` are absent,
  `mylite_sql_statistics_disabled.cc.o` is present in `libmariadbd.a`,
  ordinary `ANALYZE TABLE` and `EXPLAIN` still execute, and persistent
  statistics SQL plus variables are rejected by server-surface policy
  coverage.
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
- Re-running the embedded build without relinking does not repeatedly strip or
  resize an already-stripped archive.
- `STRIP_ARCHIVE=0` preserves an unstripped archive for diagnostics.
- The embedded archive builds with size-oriented release flags.
- Performance Schema is omitted from the embedded archive and remains omitted
  or disabled at runtime.
- Feedback reporting is omitted from the embedded archive.
- SQL `HELP` fails through the MyLite policy and the embedded MariaDB stub.
- Statement profiling is disabled in the embedded archive, profiling metadata
  is replaced by a fail-closed stub, and profiling SQL fails through the
  MyLite policy.
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
  archive omits the active binlog transaction/event core plus the unsupported
  injector root. The guarded replication execution system variables are omitted
  from `SHOW VARIABLES` and `@@` lookup in the default embedded profile.
  Replication and binlog filter variables are also omitted. PROXY protocol
  listener support is omitted, and `proxy_protocol_networks` is absent from
  `SHOW VARIABLES` and `@@` lookup in the default embedded profile. User
  statistics diagnostics are omitted, `userstat` is absent from `SHOW
  VARIABLES` and `@@` lookup, and userstat Information Schema reads plus reset
  statements are rejected. User-variable diagnostics are also omitted, ordinary
  `@variable` SQL remains covered, and user-variable diagnostic reads plus
  resets are rejected. Unix socket server authentication is omitted and the
  `unix_socket` plugin is absent. Full event parse-data validation is omitted,
  event DDL and metadata statements remain rejected, and the embedded archive
  keeps only a parser-link event parse-data stub. SQL `BINLOG` replay is
  rejected directly and in prepared statements, and `sql_binlog.cc.o` is absent
  from the embedded archive. Server-side binary-log event writers are replaced
  with a disabled embedded source, `append_query_string()` still supports
  ordinary SQL literal rendering, and `log_event_server.cc.o` is absent from
  the embedded archive. Binary-log event parser and reader runtime is omitted,
  `log_event.cc.o` is absent from the embedded archive, and retained event
  read/decode paths fail closed. Replication GTID-state runtime is replaced
  with a disabled embedded source, GTID helper SQL functions and GTID state
  variable assignments are rejected, and `rpl_gtid.cc.o` is absent from the
  embedded archive. Binary-log GTID-index runtime is replaced with a disabled
  embedded source, GTID-index tuning variables are omitted, and
  `gtid_index.cc.o` is absent from the embedded archive. SQL `HANDLER` command
  runtime is replaced with a disabled embedded source, top-level
  `HANDLER ...` statements are rejected, and `sql_handler.cc.o` is absent from
  the embedded archive. Host-file SELECT
  exports are rejected, and `SELECT ... INTO @variable` remains supported.
  Row-replication type-conversion helpers are replaced with fail-closed embedded
  stubs, `rpl_utility_server.cc.o` is absent from the embedded archive, and
  ordinary SQL type conversion remains covered through retained tests.
  Persistent
  optimizer-statistics storage is omitted, `use_stat_tables` starts as `NEVER`,
  histogram collection starts at size `0`, persistent statistics SQL and
  variable changes are rejected, and ordinary engine estimates,
  `ANALYZE TABLE`, and `EXPLAIN` remain covered.
- Process-list SHOW commands are rejected, the process-list Information Schema
  table returns zero rows, and the embedded archive omits process-list row
  producers.
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
- Statement digest normalization is omitted from the default embedded archive,
  `@@max_digest_length=0` is covered at startup, and ordinary SQL execution,
  prepared statements, diagnostics, and `EXPLAIN` remain available.
- Server status-variable publication is omitted from the default embedded
  archive, status queries return empty rows, and ordinary diagnostics,
  warnings, result metadata, and prepared statements remain available.
- The full English server error-message catalog is compacted in the default
  embedded archive, MariaDB errno and SQLSTATE remain available, syntax-error
  and duplicate-key diagnostics remain readable, and uncommon inherited server
  errors have a tested generic fallback.
- Dynamic plugin shared-object loading is omitted from the default embedded
  archive, `@@have_dynamic_loading=NO` is covered, static built-in plugins and
  native storage engines still initialize, and plugin SQL remains rejected.
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
- `log.cc`, `sql_repl.cc`, and replication utility files still have shared
  references. Removing more binlog code needs separate source and link evidence
  rather than file-name pruning.
- The disabled log-event source keeps a small virtual-method and destructor
  contract for retained unsupported paths. Further shrinking that contract
  needs link evidence plus coverage for ordinary SQL string rendering.
- The SQL `HANDLER` command runtime is omitted, but storage-engine `handler`
  classes and generic handler APIs remain required for normal table execution.
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
- Users relying on Performance Schema statement digest text or digest hashes
  lose those diagnostics in the default embedded profile. Performance Schema is
  already omitted from that profile, and ordinary statement execution remains
  available.
- Users relying on MariaDB `SHOW STATUS` counters or status Information Schema
  rows lose those server diagnostics in the default embedded profile. Ordinary
  SQL diagnostics and result metadata remain available.
- Users relying on persistent MariaDB optimizer statistics or JSON histograms
  in `mysql.table_stats`, `mysql.column_stats`, or `mysql.index_stats` lose
  that tuning metadata in the default embedded profile. Ordinary engine
  statistics, planning, `ANALYZE TABLE`, and `EXPLAIN` remain available.
- Users relying on exact full-text MariaDB server diagnostics for uncommon
  inherited errors may see generic message text in the default embedded
  profile. MariaDB errno and SQLSTATE remain the stable compatibility
  surfaces, and common MyLite-facing messages remain covered.
- Users relying on external MariaDB shared-object plugins cannot load them
  through the default embedded archive. Static built-in plugins and native
  storage engines remain available; external plugin loading requires a custom
  build profile or future adapter.
