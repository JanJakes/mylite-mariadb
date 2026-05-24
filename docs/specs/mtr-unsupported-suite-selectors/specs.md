# MTR Unsupported Suite Selectors

## Problem

The MTR coverage inventory can list exact accepted tests and exact
known-unsupported probes, but many imported upstream suites are wholly outside
the current embedded profile. The first unclassified suites include binlog,
replication, Galera/wsrep, Performance Schema, native InnoDB, and partition
tests. Adding every file in those suites as an exact unsupported row would make
the harness brittle and would hide that the decision is suite-level policy, not
a per-file probe result.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/suite/binlog/t` covers binary-log runtime behavior.
- `mariadb/mysql-test/suite/rpl/t` covers replication runtime behavior.
- `mariadb/mysql-test/suite/engines/funcs/t/rpl*.test` files either source
  `suite/rpl/t` cases directly or use replication harness primitives such as
  `include/master-slave.inc`, `sync_slave_with_master`, `CHANGE MASTER`, slave
  status, relay-log, and replication cleanup includes.
- `mariadb/mysql-test/suite/sys_vars/t` contains variable-family tests for
  disabled topology surfaces: `binlog*`, `debug_binlog*`, `expire_logs*`,
  `log_bin*`, `max_binlog*`, `read_binlog*`, `sql_log_bin*`, `sync_binlog*`,
  `gtid*`, `init_slave*`, `log_slave_updates*`,
  `master_verify_checksum*`, `pseudo_slave_mode_notembedded`, `relay*`,
  `replicate*`, `report_*`, `rpl*`, `secure_timestamp_rpl`,
  `server_id_grant`, `slave*`, `skip_parallel_replication*`,
  `skip_replication*`, `sync_master_info_grant`, `sync_relay_log*`, and
  `wsrep*`.
- `mariadb/mysql-test/suite/galera*/t` and
  `mariadb/mysql-test/suite/wsrep/t` cover Galera/wsrep runtime behavior.
- `mariadb/mysql-test/suite/perfschema*/t` covers Performance Schema runtime
  and metadata behavior.
- `mariadb/mysql-test/suite/sys_vars/t/pfs*.test` covers Performance Schema
  sizing variables, and `performance_schema*.test` covers the top-level
  Performance Schema variable.
- `mariadb/mysql-test/suite/plugins/t` covers server plugin loading,
  authentication, audit, password-check, compression-provider, and plugin
  metadata behavior.
- `mariadb/mysql-test/suite/sysschema/t` covers server-owned sys schema views
  and routines over Performance Schema, processlist, status, and native InnoDB
  observability surfaces.
- `mariadb/mysql-test/suite/versioning/t` covers system-versioned table
  metadata, history-row rewrites, transaction-registry state, and binlog,
  replication, partition, view, native InnoDB, and native engine variants.
- `mariadb/mysql-test/suite/period/t` covers application-time `PERIOD`
  metadata, `FOR PORTION OF` DML rewrites, period information-schema tables,
  trigger behavior, Performance Schema, and native InnoDB variants.
- `mariadb/mysql-test/suite/mtr/t` covers mysql-test-run overlay,
  combination, include/source, and shell self-tests.
- `mariadb/mysql-test/main/mysqltest*.test` is mixed: the curated smoke list
  keeps `mysqltest_256`, while the remaining unaccepted rows exercise the
  mysqltest runner itself, external client protocol, binlog, connection
  orchestration, expression evaluation, and tracking-info debug paths.
- `mariadb/mysql-test/main/mysql*.test`, `mysqldump*.test`, `mysqladmin.test`,
  `mysqlcheck.test`, `mysqlshow.test`, and `mysqlslap.test` exercise external
  client utilities, upgrade/install helpers, dump/restore clients, native
  MyISAM hot-copy behavior, mysql system tables, or client/server protocol
  behavior rather than the embedded core library.
- `mariadb/mysql-test/main/mysqld--*.test` and `mysqld_*.test` exercise daemon
  binary help, option parsing, startup, crash, or error behavior rather than
  the embedded core library.
- `mariadb/mysql-test/suite/stress/t` covers `mtr --stress` orchestration,
  native-engine DDL stress loops, `HANDLER` / `REPAIR TABLE` behavior, and
  long concurrent server stress paths.
- `mariadb/mysql-test/suite/large_tests/t` is explicitly documented by its
  README as long-running or disk-heavy `--big-test` coverage; the imported
  files depend on native MyISAM, encrypted Aria recovery, or replication
  surfaces.
- `mariadb/mysql-test/suite/engines/funcs/t/ld_*.test` files exercise
  `LOAD DATA INFILE`, `LOAD DATA LOCAL INFILE`, and `SELECT ... INTO OUTFILE`
  host-file SQL I/O.
- `mariadb/mysql-test/suite/engines/funcs/t/sf_*.test` and `sp_*.test`
  create stored functions or stored procedures, while `tr_*.test` creates
  triggers. `mariadb/mysql-test/suite/engines/rr_trx/t/init_innodb.test`
  sources native InnoDB bootstrap behavior with `include/have_innodb.inc`.
- `mariadb/mysql-test/suite/sys_vars/t/query_cache*.test`,
  `thread_pool*.test`, `profiling*.test`, and `userstat*.test` cover disabled
  server-observability and server-cache runtime variables.
- `mariadb/mysql-test/suite/sys_vars/t` also contains disabled server-surface
  variable families: `ssl*` sources TLS communication prerequisites;
  `named_pipe*`, `proxy_protocol_networks*`, `redirect`, and `tcp*` depend on
  network listener or client/server protocol behavior; account/admin probes
  such as `connect_timeout_grant`, `max_connections_grant`,
  `max_user_connections-2`, `proxy_user*`, `read_only_grant`,
  `secure_auth_func`, `secure_auth_grant`, and `slow_launch_time_grant` create
  users, grants, or account metadata; `init_file*`, `local_infile_func`, and
  `secure_file_priv*` depend on host SQL-file import/export configuration;
  `plugin_dir*` and `allow_suspicious_udfs` depend on plugin or UDF loading
  policy; `debug_dbug*`, `debug_mutex*`, and `debug_sync*` require debug-only
  runtime; and `secure_timestamp_super` plus `preudo_thread_id_grant` source
  replication or `BINLOG REPLAY` privilege behavior.
- `mariadb/mysql-test/suite/sys_vars/t/general_log*_func.test`,
  `log_error*.test`, `log_slow_*_func.test`, and
  `slow_query_log*_func.test` depend on daemon-owned log files or
  `mysql.slow_log` state. `sysvars_debug.test`, `sysvars_wsrep.test`, and
  `sysvars_server_notembedded.test` are profile matrices for debug, wsrep, or
  non-embedded server variables. `mdev_32254.test` and `mdev_32525.test`
  cover `redirect_url` client/server protocol and daemon-startup behavior,
  while `mdev_32640.test` covers relay-log statement execution.
- `mariadb/mysql-test/main/log_*.test` and `slowlog_*.test` are mixed: the
  curated smoke list keeps selected `log_slow_filter` and `log_state_bug33693`
  coverage, while the remaining unaccepted rows depend on daemon-owned
  general/slow log files, `mysql.general_log` and `mysql.slow_log` tables,
  daemon restart/error-log inspection, debug-only DBUG hooks, or native InnoDB
  slow-log statistics.
- `mariadb/mysql-test/suite/sys_vars/t/new_mode*.test` requires debug-only
  runtime, `read_only_func.test` creates users and checks account/admin
  behavior, `slow_launch_time_func.test` depends on one-thread-per-connection
  status metadata, and `storage_engine_basic.test` assigns native MyISAM,
  MERGE, MEMORY, and InnoDB engine names directly.
- `mariadb/mysql-test/suite/innodb*/t` covers native InnoDB engine internals,
  tablespaces, fulltext/GIS/zipped variants, and native InnoDB diagnostics.
- `mariadb/mysql-test/suite/sys_vars/t/innodb*.test` covers native InnoDB
  runtime variables.
- `mariadb/mysql-test/suite/sys_vars/t/ignore_builtin_innodb_basic.test` and
  `sysvars_innodb.test` cover native InnoDB plugin and system-variable
  metadata.
- `mariadb/mysql-test/suite/funcs_1/t/innodb_bitdata.test`,
  `innodb_cursors.test`, and `is_*_innodb.test` source native InnoDB tables or
  information-schema metadata variants, including privilege-sensitive
  non-embedded behavior.
- `mariadb/mysql-test/suite/sys_vars/t/aria_*.test` and `sysvars_aria.test`
  cover native Aria log, pagecache, encryption, recovery, repair, temporary
  table, and system-variable metadata; this is separate from MyLite routing
  application `ENGINE=Aria` metadata to the MyLite storage engine.
- `mariadb/mysql-test/suite/sys_vars/t/myisam*.test` covers native MyISAM
  runtime variables.
- `mariadb/mysql-test/suite/sys_vars/t/key_cache*.test`,
  `concurrent_insert_func.test`, `delay_key_write_func.test`, and `ft_*.test`
  cover native MyISAM key-cache, concurrent-insert, delayed-key-write, and
  FULLTEXT behavior.
- `mariadb/mysql-test/suite/funcs_1/t/myisam_bitdata.test`,
  `myisam_cursors.test`, and `is_*_myisam*.test` source native MyISAM tables
  or information-schema metadata variants, including privilege-sensitive
  non-embedded behavior.
- Exact `mariadb/mysql-test/suite/funcs_1/t/is_engines_*.test` rows for
  ARCHIVE, CSV, FEDERATED, and MRG_MYISAM depend on native or dynamically
  loaded engine registration that is outside the MyLite-routed embedded
  profile. MEMORY and BLACKHOLE engine metadata rows remain unclassified
  because those zero-file engines are MyLite compatibility targets.
- `mariadb/mysql-test/main/innodb*.test` covers native InnoDB bootstrap,
  plugin loading, optimizer, information-schema, and lock behavior. The
  embedded profile routes application `ENGINE=InnoDB` metadata to MyLite
  storage instead of registering native InnoDB.
- `mariadb/mysql-test/main/myisam*.test` covers native MyISAM table files,
  repair, recovery, key-cache, compression, and utility behavior. The current
  MyLite embedded profile routes supported application `ENGINE=MyISAM`
  metadata to MyLite storage instead of registering native MyISAM.
- `mariadb/mysql-test/main/fulltext*.test` is mixed: the curated smoke list
  keeps selected FULLTEXT syntax, Aria FULLTEXT update/cache, and Aria search
  coverage, while the remaining unaccepted files exercise native MyISAM
  FULLTEXT indexes, `MATCH ... AGAINST`, repair, and `myisam_*` variables.
  Routed MyLite storage rejects unsupported FULLTEXT indexes explicitly.
- `mariadb/mysql-test/suite/parts/t` covers the partition engine.
- `mariadb/mysql-test/main/partition*.test` covers partition DDL, pruning,
  management, and partition-specific native-engine behavior.
- `mariadb/mysql-test/suite/engines/funcs/t/tc_partition*.test` covers
  partition table-control DDL and management behavior.
- Additional exact `mariadb/mysql-test/main` partition-related probes outside
  the `partition*` family cover disabled partition DDL and metadata through
  charset, EXPLAIN/ANALYZE, identifier-collation, information-schema,
  sargability, and statistics paths: `ctype_partitions.test`,
  `ctype_uca_partitions.test`, `explain_json_format_partitions.test`,
  `identifier_partition.test`, `information_schema_part.test`,
  `not_partition.test`, `sargable_casefold_part.test`, and
  `stat_tables_partition.test`.
- `mariadb/mysql-test/main` contains clear disabled server-surface families:
  `grant*`, `user_limits`, and `password_expiration*` account tests; `ssl*`
  network TLS tests; `plugin*` dynamic plugin and plugin-auth tests; `backup*`
  external backup SQL tests; `query_cache*`; `mysqlbinlog*` and `rpl_*`;
  `loaddata*` and `outfile*`; `servers*` foreign-server metadata tests;
  `processlist*`; `udf*`; `xa*`; `userstat*`; `vector*`; and `gis*`.
- Additional `mariadb/mysql-test/main` account and privilege probes are
  outside the embedded profile: `alter_user.test`, `create_user.test`,
  `create_drop_user.test`, `create_drop_role.test`, `change_user*.test`,
  `cte_grant.test`, `analyze_stmt_privileges*.test`, and
  `create_or_replace_permission.test` create users or roles, manipulate grant
  tables, check account authentication, or assert privilege-sensitive protocol
  session behavior. The same applies to `delete_returning_grant.test`,
  `failed_auth_3909.test`, `failed_auth_unixsocket.test`,
  `fix_priv_tables.test`, and `lock_user.test`.
- Additional `mariadb/mysql-test/main` client/server protocol probes are
  outside the embedded profile: `connect.test`, `connect2.test`,
  `connect-abstract.test`, `connect_debug.test`,
  `bind_address_resolution.test`, `bind_multiple_addresses_resolution.test`,
  `auth_named_pipe.test`, `cli_options_force_protocol_*.test`,
  `chained_ssl_certificates.test`, `client*.test`, `compress.test`, and
  `check_view_protocol.test` depend on daemon listeners, external client
  commands, protocol modes, TLS/named-pipe authentication, compressed
  connections, or mysqltest protocol/status behavior.
- Additional `mariadb/mysql-test/main` server-feature DDL probes are outside
  the embedded profile: `alter_events.test` and `create_drop_event.test`
  require the event scheduler and event metadata; `create_drop_binlog.test`
  requires binary-log event recording; `create_drop_function.test` requires
  stored-function runtime and `mysql.proc`; `create_drop_server.test` requires
  `mysql.servers`; `create_drop_udf.test` requires dynamic UDF loading and
  `mysql.func`; `create_or_replace_pfs.test` requires Performance Schema and
  sys schema metadata; and `blackhole_plugin.test` requires dynamic BLACKHOLE
  plugin install/uninstall behavior.
- Additional `mariadb/mysql-test/main` daemon/client utility probes are
  outside the embedded profile: `bad_startup_options*.test`,
  `bootstrap*.test`, `large_pages.test`, `mariadb-upgrade-service.test`,
  `winservice_basic.test`, `winservice_i18n.test`,
  `mariadb-dump-debug.test`, and `mariadb-import.test` start standalone
  daemon/bootstrap/service flows, daemon option profiles, or external
  dump/import helpers. `bad_frm_crash_5029.test` copies legacy `.frm` plus
  native Aria `.MAI` / `.MAD` sidecars into the datadir.
- Additional exact `mariadb/mysql-test/main` server-surface probes remain
  outside the embedded profile: `auth_rpl.test` depends on replication plus
  plugin-auth accounts; `kill*.test` depends on daemon connection management
  and processlist state; `handlersocket.test` installs a plugin; `help.test`
  depends on SQL `HELP` and help-table metadata; `func_analyse.test` depends
  on `PROCEDURE ANALYSE()`; `sequence_debug.test` depends on sequence runtime;
  `show_profile.test` depends on statement profiling; and
  `custom_aggregate*.test`, `information_schema_parameters.test`, and
  `information_schema_routines.test` create stored routines or inspect routine
  metadata.
- Additional exact `mariadb/mysql-test/main` administrative probes are outside
  the embedded profile: `analyze_stmt_slow_query_log.test` depends on daemon
  slow-query log files; `assign_key_cache*.test` depends on native MyISAM
  key-cache administration; `key_cache.test`, `preload.test`, and
  `select_pkeycache.test` depend on native key-cache or `LOAD INDEX INTO CACHE`
  behavior; `repair*.test` depends on native MyISAM/Aria sidecar repair;
  `flush*.test` and `deadlock_ftwrl.test` depend on daemon administrative
  FLUSH/FTWRL behavior; `dirty_close.test` depends on mysqltest dirty-close
  session handling and processlist state; `information_schema_chmod.test`
  mutates datadir permissions; and `frm-debug.test`,
  `frm_bad_row_type-7333.test`, `huge_frm-6224.test`, and
  `temp_table_frm.test` depend on persistent `.frm` metadata, daemon restarts,
  or native sidecars.
- Additional exact `mariadb/mysql-test/main` optional-function and profile
  probes are outside the embedded profile: `func_compress.test` and
  `column_compression*.test` depend on zlib-backed SQL or compressed-column
  storage; `func_sformat.test`, `func_des_encrypt.test`, and
  `func_encrypt*.test` depend on disabled optional SQL functions;
  `create_delayed.test`, `delayed.test`, and
  `delayed_blob.test` depend on `INSERT DELAYED`; `contributors.test` depends
  on static SHOW metadata; `file_contents.test` checks packaging files; and
  `aborted_clients.test`, `memory_used.test`, `handler_read_last.test`, and
  `host_cache_size_functionality.test` depend on daemon status, processlist,
  host-cache, or restart behavior.
- Additional exact `mariadb/mysql-test/main` debug probes are outside the
  release embedded profile when they source `include/have_debug.inc`,
  `include/have_debug_sync.inc`, or debug-only helper includes, or mutate
  `debug_dbug` / `debug_sync`. The selector set deliberately leaves
  `table_elim_debug.test` unclassified because its debug include is commented
  out and the remaining body exercises ordinary optimizer-switch behavior.
  `func_json_notembedded.test` requires debug-only `max_statement_time` hooks,
  profiling, and non-embedded connection orchestration.
- Additional exact `mariadb/mysql-test/suite/optimizer_unfixed_bugs` probes
  are outside the release embedded profile when they source
  `include/have_debug.inc` for debug-only optimizer hooks, and many also
  depend on native InnoDB, binary-log state, or valgrind/big-test debug
  coverage.
- Additional exact `mariadb/mysql-test/main` profiling probes are outside the
  embedded profile: `profiling.test`, `nested_profiling.test`, and
  `set_statement_profiling.test` source `include/have_profiling.inc` and
  exercise `SHOW PROFILE`, `INFORMATION_SCHEMA.PROFILING`, profiling system
  variables, or profiling with `init_connect` and server accounts.
- Additional exact `mariadb/mysql-test/main` file-lifecycle probes are outside
  the embedded profile: `init_file*.test` depends on `--init-file` startup
  SQL-file execution and daemon restart state, `loadxml.test` depends on host
  XML file import and dump-file round trips, `ctype_gbk_export_import.test`
  depends on SELECT/LOAD host-file round trips, `LOAD_FILE()`, external
  mysql/mysqldump clients, views, and routines, `secure_file_priv_win.test`
  depends on `LOAD_FILE()` and `LOAD DATA` host paths, and `symlink*.test` plus
  `temp_table_symlink.test` depend on native MyISAM/Aria symlinked sidecar
  files.
- Additional exact `mariadb/mysql-test/main` network, TLS, thread-handling,
  daemon-control, and protocol probes are outside the embedded profile:
  IPv4/IPv6 and named pipe tests depend on daemon listeners, TLS tests source
  SSL communication prerequisites or execute external SSL clients, thread
  tests depend on daemon thread-handling or thread-pool modes,
  `mdev-21101.test` misconfigures the daemon thread pool around client
  connection timeouts, `default_storage_engine.test`, `lowercase_fs_on.test`,
  `large_pages.test`, `shutdown.test`, and `sighup-6580.test` stop, restart,
  bootstrap, configure, or signal the daemon, Windows service tests install
  and manage standalone daemon services, and non-blocking, packet-size,
  idle-transaction-timeout, and reset-connection tests depend on client/server
  protocol behavior.
- Additional exact `mariadb/mysql-test/main` binlog, replication, and
  query-cache probes are outside the embedded profile: charset and user-variable
  binlog tests source binary-log formats, `SHOW BINLOG EVENTS`, or
  `mysqlbinlog`, `stat_tables_rbr.test` depends on row-format binary logs and
  `SHOW BINLOG EVENTS`, replication-named tests source master/slave
  orchestration or `CHANGE MASTER`, `stat_tables_repl.test` depends on
  master/slave statistics-table replication, and query-cache tests source
  `include/have_query_cache.inc` and mutate query-cache state.
- Additional exact `mariadb/mysql-test/main` optimizer-trace probes are outside
  the embedded profile because they set `optimizer_trace` and read
  `INFORMATION_SCHEMA.OPTIMIZER_TRACE`: `distinct_notembedded.test`,
  `group_min_max_notembedded.test`, `index_merge_innodb_notembedded.test`,
  `range_notembedded.test`, `sargable_casefold_notembedded.test`, and
  `selectivity_notembedded.test`, plus existing `opt_trace*.test` probes.
  Some also depend on native InnoDB, Sequence, grants, views, or stored
  functions.
- Additional exact `mariadb/mysql-test/main` status and static metadata probes
  are outside the embedded profile: `status*.test` and
  `max_session_mem_used.test` depend on daemon/session status counters,
  processlist state, or `FLUSH STATUS`; `show_analyze*.test` and
  `show_explain*.test` depend on daemon session inspection, debug sync,
  processlist, or Performance Schema state; `bug12427262.test` and
  `information_schema_all_engines.test` depend on Performance Schema and
  native file/engine metadata; `information_schema-big*.test` depends on CSV
  log tables and native InnoDB profile metadata; `information_schema_linux.test`
  depends on processlist thread metadata; `information_schema_stats.test`
  depends on user-statistics runtime; `information_schema_columns.test` and
  `information_schema_tables.test` depend on stored functions, views, named
  locks, and protocol sessions; `truncate_notembedded.test` depends on
  connection/status metadata plus native MyISAM locking; `show_create_user.test`
  depends on server account metadata; `show_bad_definer-5553.test` depends on
  view and definer metadata; `show_function_with_pad_char_to_full_length.test`
  depends on stored-function metadata; and `session_tracker_sysvar.test` plus
  `variables-notembedded.test` depend on non-embedded GTID, binlog, relay-log,
  or replication variables. `variables_community.test` depends on global
  status uptime counters and `FLUSH STATUS` behavior.
- Additional exact `mariadb/mysql-test/main` account, daemon-log, system-table
  upgrade, and process-management probes are outside the embedded profile:
  `bug58669.test`, `cte_nonrecursive_not_embedded.test`,
  `information_schema.test`, `information_schema_db.test`,
  `init_connect.test`, `invisible_field_grant*.test`,
  `lowercase_fs_off.test`, `lowercase_table_grant.test`,
  `max_password_errors.test`, `not_embedded_server.test`, `ps_grant.test`,
  `public_basic.test`, `public_privileges.test`, `read_only*.test`,
  `session_user.test`, `set_password.test`, `skip_grants.test`,
  `skip_name_resolve.test`, `system_mysql_db_507.test`,
  `system_mysql_db_error_log.test`, and `timezone_grant.test` source
  `include/not_embedded.inc` and depend on server accounts, grants,
  authentication, privilege-table mutation, read-only-admin/global read-only
  policy, protocol sessions, or skip-grant-table daemon state; selected
  `ctype_upgrade.test`, `statistics_upgrade.test`,
  `statistics_upgrade_not_done.test`, `system_mysql_db_fix*.test` files, and
  `upgrade*.test` run external upgrade or mysqlcheck tooling over legacy native
  datadir, grant, routine, and view fixtures; `bug47671.test`,
  `ctype_utf32_not_embedded.test`,
  `delimiter_command_case_sensitivity.test`, and
  `parser_not_embedded.test` run external client or dump tooling over
  client/server protocol, event runtime, or parser scripts;
  `system_mysql_db.test` depends on native CSV log tables in the bootstrap
  `mysql` schema; `ps_show_log.test` and prepared missed-command tests depend
  on master/slave or event/relay-log surfaces; `explain_slowquerylog.test` and
  `mdev_19276.test` depend on daemon log inspection; `lock_kill.test`,
  `thread_id_overflow.test`, and `mdev375.test` depend on KILL/processlist or
  daemon connection/status accounting; `tmp_table_count-7586.test` depends on
  Performance Schema and status counters; `empty_server_name-8224.test` depends
  on foreign-server metadata plus daemon restart; and `perror-win.test`
  executes the external `perror` utility.
- Additional Windows-only probes are outside the embedded profile when they
  depend on Windows console code pages, MAX_PATH filename behavior, device-name
  filesystem semantics, or Windows service and host utility execution:
  `charset_client_win.test`, `create_windows.test`, `windows.test`, and the
  `winservice*.test` service probes. The similarly named `win*.test` files
  that cover SQL window functions remain unclassified or accepted separately
  based on their own compatibility value.
- Additional exact online/in-place DDL probes are outside MyLite's current
  copy-ALTER profile: `alter_table_locknone.test`,
  `alter_table_locknone_notembedded.test`, `alter_table_online.test`, and
  `vcol/alter_inplace-9045.test` depend on `LOCK=NONE`,
  `ALTER ONLINE TABLE`, `ALGORITHM=INPLACE`, `ALGORITHM=NOCOPY`, native engine,
  binlog, Sequence, system-versioning, or daemon session behavior. MyLite
  already rejects representative online/in-place ALTER requests explicitly.
- Additional generated/virtual-column suite probes are outside the embedded
  profile when they exercise disabled surrounding surfaces: `json_table*.test`
  depends on `JSON_TABLE()`, `rpl_json*.test`, `rpl_vcol.test`, and
  `rpl_gcol.test` depend on replication, `vcol/*binlog*.test` depends on the
  binary log, `vcol/query_cache.test` depends on the query cache,
  `vcol/load_data.test` depends on host-file SQL I/O, `vcol/delayed.test`
  depends on `INSERT DELAYED`, generated/virtual-column partition tests depend
  on the partition engine, generated/virtual-column trigger/stored-program
  tests depend on trigger and stored-program runtime, generated/virtual-column
  view tests depend on view runtime, and `gcol/main_mysqldump.test` depends on
  external dump tooling. Exact generated/virtual-column optional-engine probes
  for ARCHIVE, CSV, and MERGE/MRG_MyISAM depend on native optional engine
  registration outside MyLite routed storage. Exact generated/virtual-column
  HANDLER probes execute SQL `HANDLER` commands that are intentionally disabled
  in the embedded profile. Exact generated-column InnoDB leftovers
  `gcol_purge.test`, `gcol_rollback.test`, and `virtual_index_drop.test`
  require debug hooks, debug-sync orchestration, native InnoDB purge, restart,
  or online ALTER behavior; `innodb_virtual_debug.test`,
  `innodb_virtual_debug_purge.test`, and `innodb_virtual_rebuild.test` require
  debug hooks and native InnoDB online rebuild/purge behavior;
  `innodb_virtual_fk_restart.test`,
  `innodb_virtual_purge.test`, and `innodb_virtual_stats.test` depend on native
  InnoDB restart, purge, persistent-stats, or in-place virtual-column ALTER
  behavior. Broader generated-column InnoDB coverage such as
  `innodb_virtual_basic.test`, `innodb_virtual_fk.test`, and
  `innodb_virtual_index.test` remains unclassified for future routed MyLite
  compatibility work.
  Exact virtual-column leftovers `upgrade.test` and
  `vcol_sql_mode_upgrade.test` copy legacy `.frm`, `.MYD`, `.MYI`, and
  partition sidecar fixtures into the datadir; `vcol_keys_myisam.test` depends
  on native MyISAM indexes, `myisamchk`, and repair;
  `myisam_repair_prefix_varchar.test` depends on native MyISAM `REPAIR TABLE`;
  and `races.test`, `vcol_misc.test`, plus `vcol_sargable_debug.test` require
  debug runtime hooks. Ordinary virtual column sargability and index behavior
  remains unclassified for future routed coverage.
- `mariadb/mysql-test/main/view*.test`, `lock_view.test`, `trigger*.test`, and
  `mdev-34724.test` cover view and trigger runtime plus metadata or host-file
  surfaces that are disabled in the embedded profile.
- `mariadb/mysql-test/main/sp*.test` broadly covers stored-procedure runtime.
  The unsupported selectors deliberately exclude curated accepted smoke rows
  such as `sp-memory-leak`, `sp-no-code`, and `sp_missing_4665`.
- Additional exact stored-program and account/runtime leftovers are outside
  the embedded profile: `cte_recursive_not_embedded.test` creates a stored
  function and coordinates `KILL QUERY` through daemon protocol sessions,
  `func_rollback.test` exercises stored functions that modify tables and
  views, and `insert_notembedded.test` depends on server accounts, grants,
  SQL SECURITY views, trigger runtime, `INSERT DELAYED`, and protocol user
  switching.
- `mariadb/mysql-test/suite/funcs_1/t` contains disabled routine, trigger,
  view, and processlist families: `*_storedproc_*` and `storedproc` source
  stored-procedure include files; `*_trig*` covers engine-specific trigger
  cases plus information-schema trigger metadata probes; `*_func_view`,
  `*_views*` cover function-in-view and view runtime plus information-schema
  view metadata probes; and `processlist*` depends on daemon process-list
  metadata, with upstream comments explicitly noting that the no-protocol
  variants do not make sense under embedded server because the processlist is
  empty.
- Additional exact `mariadb/mysql-test/suite/funcs_1/t` information-schema
  probes are outside the embedded profile: `is_*_privileges*.test` sources
  `include/not_embedded.inc` and creates users, grants, or protocol sessions;
  `is_events.test` covers event metadata; and `is_routines*.test` sources
  stored routine creation plus routine privilege checks.
- Additional exact `mariadb/mysql-test/suite/funcs_1/t` privilege-sensitive
  information-schema metadata probes are outside the embedded profile:
  `charset_collation.test`, `is_basics_mixed.test`,
  `is_check_constraints.test`, selected `is_columns*.test`,
  `is_key_column_usage.test`, selected `is_schemata*.test`,
  selected `is_statistics*.test`, selected `is_table_constraints*.test`, and
  selected `is_tables*.test` create low-privilege accounts or source
  `include/not_embedded.inc` because expected rows depend on server accounts,
  grants, protocol sessions, or privilege-filtered metadata. Embedded variants
  and MEMORY/BLACKHOLE engine metadata rows remain unclassified unless their
  source has separate unsupported-surface evidence.
- Additional exact profile and plugin probes are outside executable embedded
  coverage: `sys_vars/all_vars.test` installs optional storage-engine plugins
  to check the full server variable inventory, `sys_vars/sysvars_star.test`
  installs `sql_errlog` and checks system-variable origin metadata through
  accounts and protocol sessions, `sys_vars/transaction_prealloc_size_bug27322`
  is `--big-test` coverage that may allocate 5GB and inspects
  `SHOW PROCESSLIST`, `func_json_notembedded.test` requires debug-only
  `max_statement_time` hooks, `ps_not_windows.test` and
  `table_options-5867.test` require dynamic plugin loading, and
  `funcs_1/memory_bitdata.test` plus
  `funcs_1/memory_cursors.test` are upstream placeholders that immediately
  exit after reporting `NOT YET IMPLEMENTED`.
- Additional exact legacy datadir probes are outside the single-file embedded
  profile: `ctype_utf8_def_upgrade.test` copies legacy MyISAM `.frm`, `.MYD`,
  `.MYI`, and `db.opt`-style fixtures; `type_temporal_mariadb53.test` and
  `type_temporal_mysql56.test` copy legacy MariaDB/MySQL temporal metadata
  fixtures; and `type_varchar_mysql41.test` copies old varchar `.frm` fixtures
  before native repair/check/ALTER upgrade paths.
- Additional exact `mariadb/mysql-test/main` big-test probes are outside the
  current curated embedded smoke profile because they source
  `include/big_test.inc`: `alter_table-big.test`,
  `analyze_format_json_timings.test`, `create-big.test`,
  `delete_use_source.test`, `long_unique_big.test`, `lowercase_table4.test`,
  `order_by_pack_big.test`, `read_many_rows_innodb.test`,
  `selectivity_innodb*.test`, `sum_distinct-big.test`,
  `tmp_space_usage.test`, and `type_newdecimal-big.test`.
- The imported `compat` suite files live under
  `mariadb/mysql-test/suite/compat/oracle/t`; they are Oracle-compatibility
  MTR cases.
- `mariadb/mysql-test/suite/encryption/t` covers key-management plugin files,
  native InnoDB and Aria encryption, binlog encryption, encrypted temporary
  files, and native recovery/import behavior.
- `docs/ROADMAP.md` and `docs/COMPATIBILITY.md` already describe binlog,
  replication/Galera, Performance Schema, server plugin surfaces, sys schema
  observability, native InnoDB, partitioning, and Oracle SQL mode as disabled,
  trimmed, or explicitly unsupported surfaces in the current embedded profile.
  MariaDB native
  encryption plugins, engine-specific encrypted sidecar behavior, temporal
  table metadata, test-runner self-tests, long stress loops, and long/disk-heavy
  native-engine tests are not part of the current MyLite single-file storage
  contract.

## Design

Extend `tools/mylite-mtr-harness` with unsupported suite selectors:

- exact `unsupported_tests` entries continue to describe probed individual
  files;
- new `unsupported_test_selectors` entries use shell-style imported test-name
  patterns such as `binlog.*`;
- `list-unsupported` expands selectors against the imported MTR inventory and
  prints concrete `suite.test` rows with their reason category;
- `coverage` and `list-unclassified` consume the expanded unsupported names,
  so selector-covered imported tests are removed from the unclassified count;
- the existing overlap guard remains: selector expansion must not overlap
  accepted curated tests.

The selector set is deliberately limited to whole suites or narrow name
families whose runtime is outside MyLite's current embedded profile. Mixed
areas such as broad `main`, broad `sys_vars`, broad `engines`, charset suites,
JSON suites, and information-schema matrices stay unclassified until they are
accepted, probed, or classified by a source-backed selector.

## Compatibility Impact

This is inventory bookkeeping only. It does not mark the selected upstream MTR
files as accepted compatibility coverage and does not claim support for the
underlying server surfaces. It makes deliberate non-coverage visible so future
MTR expansion does not repeatedly rediscover the same out-of-profile suites.

## Single-File And Embedded Lifecycle Impact

No runtime or file-format change. The classified suites are outside the current
single-file embedded profile because they depend on server logs, replication,
native engines, partition metadata, or Performance Schema state.

## Public API And File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency and no binary-size change. The harness remains a Bash script.

## Test And Verification Plan

- Run `bash -n tools/mylite-mtr-harness`.
- Run `tools/mylite-mtr-harness list-unsupported` and confirm selector-covered
  suites expand to concrete rows.
- Run `tools/mylite-mtr-harness coverage` and record the updated unsupported
  and unclassified counts.
- Run `tools/mylite-mtr-harness list-unclassified` and confirm no selected
  suite remains in the output.
- Run `git diff --check`.

## Verification Evidence

- `bash -n tools/mylite-mtr-harness`: passed.
- `tools/mylite-mtr-harness coverage`: accepted upstream coverage stayed at
  413 of 5,901 imported upstream files, known unsupported upstream files became
  4,606, and unclassified upstream files dropped to 882.
- `tools/mylite-mtr-harness list-unsupported` expanded the selector-backed
  categories to concrete rows:
  - `replication-surface`: 860 rows.
  - `disabled-galera-runtime`: 709 rows.
  - `native-innodb-profile`: 712 rows.
  - `disabled-performance-schema`: 499 rows.
  - `disabled-binlog-runtime`: 222 rows.
  - `disabled-partition-engine`: 234 rows.
  - `disabled-oracle-mode`: 89 rows, including 87 selector-expanded `compat`
    rows plus two earlier exact probes.
  - `disabled-native-encryption-profile`: 73 rows.
  - `disabled-file-io`: 33 rows, including selector-expanded `engines.ld_*`,
    `main.loaddata*`, and `main.outfile*` rows plus earlier exact probes.
  - `native-engine-profile`: 113 rows, including selector-expanded native
    Aria, RocksDB temporary-engine, `sys_vars.myisam*`, `main.myisam*`, and
    selected unaccepted `main.fulltext*`, funcs_1 MyISAM/engine metadata, and
    main symlink/upgrade/legacy datadir sidecar rows plus earlier exact probes.
  - `native-myisam-sysvar`: 15 rows.
  - `disabled-query-cache`: 24 rows.
  - `disabled-thread-pool`: 15 rows.
  - `disabled-statement-profiling`: 6 rows.
  - `disabled-optimizer-trace`: 12 rows.
  - `disabled-user-statistics`: 4 rows.
  - `disabled-plugin-surface`: 61 rows.
  - `server-account-surface`: 103 rows, including exact `funcs_1`
    information-schema privilege and privilege-filtered metadata rows.
  - `network-tls-surface`: 33 rows.
  - `network-listener-surface`: 23 rows.
  - `server-log-surface`: 17 rows.
  - `disabled-log-table`: 13 rows.
  - `external-backup-surface`: 10 rows.
  - `disabled-vector-surface`: 10 rows.
  - `disabled-gis-surface`: 9 rows.
  - `disabled-udf-runtime`: 7 rows.
  - `disabled-xa-runtime`: 4 rows.
  - `foreign-server-metadata`: 4 rows.
  - `disabled-processlist-metadata`: 21 rows, including selector-expanded
    `funcs_1.processlist*` rows plus exact main processlist, KILL,
    SHOW ANALYZE, and SHOW EXPLAIN probes.
  - `disabled-stored-program-runtime`: 81 rows, including exact
    `funcs_1.is_routines*` metadata rows.
  - `disabled-trigger-runtime`: 44 rows, including engine-specific trigger
    rows plus `funcs_1.is_triggers*` metadata rows.
  - `disabled-view-runtime`: 18 rows, including function-in-view, view runtime,
    and `funcs_1.is_views*` metadata rows.
  - `disabled-sys-schema-surface`: 93 rows.
  - `unsupported-temporal-table-surface`: 43 rows.
  - `big-test-profile`: 20 rows, including 4 selector-expanded `large_tests`
    rows plus exact main and sys_vars probes.
  - `stress-runner-profile`: 8 rows.
  - `mtr-runner-selftest`: 14 rows.
  - `client-utility-profile`: 82 rows.
  - `daemon-utility-profile`: 17 rows.
  - `protocol-profile`: 6 rows.
  - `disabled-server-utility-function`: 4 rows.
  - `disabled-event-surface`: 5 rows.
  - `disabled-select-procedure`: 1 row.
  - `disabled-help-surface`: 1 row.
  - `disabled-sequence-runtime`: 1 row.
  - `disabled-flush-surface`: 15 rows.
  - `disabled-table-maintenance`: 7 rows.
  - `unsupported-online-ddl-profile`: 4 rows.
  - `debug-only`: 93 rows, including exact main-suite, generated/virtual-column,
    and optimizer-unfixed probes that source debug-only runtime hooks.
  - `disabled-status-metadata`: 19 rows.
  - `disabled-zlib-compression`: 5 rows.
  - `disabled-delayed-insert`: 6 rows.
  - `disabled-json-table-function`: 4 rows.
  - `disabled-sformat-function`: 1 row.
  - `disabled-des-function`: 3 rows.
  - `disabled-sql-handler`: 5 rows.
  - `disabled-static-show-info`: 1 row.
  - `packaging-profile`: 1 row.
  - `platform-skip`: 5 rows.
  - `embedded-skip`: 26 rows.
  - `upstream-disabled`: 3 rows.
- `tools/mylite-mtr-harness list-unclassified` no longer prints tests from
  `binlog`, `rpl`, `galera`, `galera_sr`, `galera_3nodes`,
  `galera_3nodes_sr`, `wsrep`, `perfschema`, `perfschema_stress`, `innodb`,
  `innodb_fts`, `innodb_gis`, `innodb_zip`, `parts`, `compat`, `plugins`,
  `encryption`, `sysschema`, `versioning`, `period`, `mtr`, `stress`, or
  `large_tests`; it also no longer prints `engines.rpl*` or `engines.ld_*`
  tests, nor `sys_vars` tests whose names start with `binlog`, `gtid`,
  `innodb`, `pfs`, `relay`, `replicate`, `rpl`, `slave`, or `wsrep`, nor
  `sys_vars` topology tests whose names start with `debug_binlog`,
  `expire_logs`, `log_bin`, `max_binlog`, `read_binlog`, `sql_log_bin`,
  `sync_binlog`, `init_slave`, `log_slave_updates`, `master_verify_checksum`,
  `report_`, `skip_parallel_replication`, `skip_replication`, or
  `sync_relay_log`, nor exact `sys_vars.pseudo_slave_mode_notembedded`,
  `sys_vars.secure_timestamp_rpl`, `sys_vars.server_id_grant`, and
  `sys_vars.sync_master_info_grant` tests, nor selected `sys_vars` server
  surface tests for account/admin privileges, file-I/O, plugin/UDF loading,
  TLS/network listener behavior, debug-only variables,
  `sys_vars.secure_timestamp_super`, and `sys_vars.preudo_thread_id_grant`,
  nor selected `sys_vars` tests for native Aria, InnoDB, RocksDB temporary
  engine, MyISAM key-cache, concurrent-insert, delayed-key-write, and FULLTEXT
  system variables,
  nor selected `sys_vars` tests for daemon-owned logs, profile matrices,
  redirect_url regressions, or relay-log statement execution,
  nor exact `sys_vars` tests for plugin inventory, plugin-origin metadata, and
  big processlist preallocation coverage,
  nor exact `sys_vars` tests for debug-only `new_mode`, native
  `storage_engine`, server-account `read_only`, or slow-launch status
  behavior, nor exact main-suite debug-only, profiling, host-file startup /
  import, symlink sidecar, network/TLS, thread-handling, protocol, binlog,
  replication, query-cache, optimizer-trace, status, account, view, routine,
  SHOW EXPLAIN / SHOW ANALYZE, session-tracker, grant/account,
  slow-query-log, foreign-server restart, KILL/processlist debug, and external
  `perror`, client, dump, parser, and upgrade utility probes, nor exact main
  `--big-test` probes, native key-cache/preload probes, query-cache InnoDB
  probe, native MyISAM and legacy charset/temporal/varchar upgrade fixture
  probes, Windows-only probes, large-page daemon profile probe,
  legacy `.frm` / datadir permission probes, and selected funcs_1 engine
  metadata probes, nor exact information-schema log-table, processlist,
  user-statistics, view, and stored-function metadata probes,
  nor `engines` stored-procedure, stored-function, trigger, or native InnoDB
  bootstrap tests,
  nor `main.view*` or `main.trigger*` tests,
  nor selected `main.sp*` stored-procedure tests outside the curated accepted
  smoke rows,
  nor `main.partition*`, exact main partition-related charset, EXPLAIN,
  information-schema, identifier, sargability, statistics, and disabled
  partition probes, or `engines.tc_partition*` tests, nor `sys_vars` tests
  whose names start with `myisam`, `performance_schema`, `profiling`,
  `query_cache`, `thread_pool`, or `userstat`, nor `main` tests from the
  account, TLS, plugin, backup, query-cache, binlog, replication, file-I/O,
  foreign-server, processlist, UDF, XA, userstat, vector, or GIS families
  listed above, nor `funcs_1` stored-program, trigger, view, processlist,
  privilege metadata, privilege-filtered metadata, event metadata, or routines
  metadata families, nor exact upstream-disabled funcs_1 MEMORY placeholders.
- `git diff --check`: passed.

## Acceptance Criteria

- Exact unsupported probes remain supported.
- Suite selectors expand only to imported upstream MTR tests.
- Accepted curated MTR tests do not overlap unsupported selectors.
- Coverage counts distinguish accepted tests from known unsupported
  non-coverage.
- Documentation uses "probes and selectors" wording instead of implying every
  unsupported row was individually run.

## Risks

- A future MyLite profile may want to accept a narrow test from a selector
  covered suite. The existing overlap guard will catch that if the test is
  added to the accepted list; the selector must then be narrowed or removed.
- Whole-suite classification is appropriate only for suites with uniform
  out-of-profile prerequisites. Mixed suites must continue to use exact probes
  or narrower selectors.
