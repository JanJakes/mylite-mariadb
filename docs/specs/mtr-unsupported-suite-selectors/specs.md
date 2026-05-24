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
  `bootstrap*.test`, `mariadb-upgrade-service.test`,
  `mariadb-dump-debug.test`, and `mariadb-import.test` start standalone
  daemon/bootstrap/service flows or execute external dump/import helpers.
  `bad_frm_crash_5029.test` copies legacy `.frm` plus native Aria `.MAI` /
  `.MAD` sidecars into the datadir.
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
  key-cache administration; `repair*.test` depends on native MyISAM/Aria
  sidecar repair; `flush*.test` and `deadlock_ftwrl.test` depend on daemon
  administrative FLUSH/FTWRL behavior; `dirty_close.test` depends on mysqltest
  dirty-close session handling and processlist state; and `frm-debug.test`,
  `frm_bad_row_type-7333.test`, and `huge_frm-6224.test` depend on persistent
  `.frm` metadata or native sidecars.
- Additional exact `mariadb/mysql-test/main` optional-function and profile
  probes are outside the embedded profile: `func_compress.test` and
  `column_compression*.test` depend on zlib-backed SQL or compressed-column
  storage; `func_sformat.test` and `func_des_encrypt.test` depend on disabled
  optional SQL functions; `create_delayed.test`, `delayed.test`, and
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
- Additional exact `mariadb/mysql-test/main` profiling probes are outside the
  embedded profile: `profiling.test`, `nested_profiling.test`, and
  `set_statement_profiling.test` source `include/have_profiling.inc` and
  exercise `SHOW PROFILE`, `INFORMATION_SCHEMA.PROFILING`, profiling system
  variables, or profiling with `init_connect` and server accounts.
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
  external dump tooling.
- `mariadb/mysql-test/main/view*.test` and `trigger*.test` cover view and
  trigger runtime plus metadata surfaces that are disabled in the embedded
  profile.
- `mariadb/mysql-test/main/sp*.test` broadly covers stored-procedure runtime.
  The unsupported selectors deliberately exclude curated accepted smoke rows
  such as `sp-memory-leak`, `sp-no-code`, and `sp_missing_4665`.
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
  4,318, and unclassified upstream files dropped to 1,170.
- `tools/mylite-mtr-harness list-unsupported` expanded the selector-backed
  categories to concrete rows:
  - `replication-surface`: 853 rows.
  - `disabled-galera-runtime`: 709 rows.
  - `native-innodb-profile`: 709 rows.
  - `disabled-performance-schema`: 497 rows.
  - `disabled-binlog-runtime`: 212 rows.
  - `disabled-partition-engine`: 226 rows.
  - `disabled-oracle-mode`: 89 rows, including 87 selector-expanded `compat`
    rows plus two earlier exact probes.
  - `disabled-native-encryption-profile`: 73 rows.
  - `disabled-file-io`: 27 rows, including selector-expanded `engines.ld_*`,
    `main.loaddata*`, and `main.outfile*` rows plus earlier exact probes.
  - `native-engine-profile`: 89 rows, including selector-expanded native
    Aria, RocksDB temporary-engine, `sys_vars.myisam*`, `main.myisam*`, and
    selected unaccepted `main.fulltext*` and funcs_1 MyISAM metadata rows
    plus earlier exact probes.
  - `native-myisam-sysvar`: 15 rows.
  - `disabled-query-cache`: 19 rows.
  - `disabled-thread-pool`: 11 rows.
  - `disabled-statement-profiling`: 6 rows.
  - `disabled-user-statistics`: 3 rows.
  - `disabled-plugin-surface`: 57 rows.
  - `server-account-surface`: 59 rows, including exact `funcs_1`
    information-schema privilege metadata rows.
  - `network-tls-surface`: 26 rows.
  - `network-listener-surface`: 17 rows.
  - `server-log-surface`: 15 rows.
  - `disabled-log-table`: 10 rows.
  - `external-backup-surface`: 10 rows.
  - `disabled-vector-surface`: 10 rows.
  - `disabled-gis-surface`: 9 rows.
  - `disabled-udf-runtime`: 7 rows.
  - `disabled-xa-runtime`: 4 rows.
  - `foreign-server-metadata`: 3 rows.
  - `disabled-processlist-metadata`: 12 rows, including selector-expanded
    `funcs_1.processlist*` rows plus exact main processlist and KILL probes.
  - `disabled-stored-program-runtime`: 74 rows, including exact
    `funcs_1.is_routines*` metadata rows.
  - `disabled-trigger-runtime`: 43 rows, including engine-specific trigger
    rows plus `funcs_1.is_triggers*` metadata rows.
  - `disabled-view-runtime`: 16 rows, including function-in-view, view runtime,
    and `funcs_1.is_views*` metadata rows.
  - `disabled-sys-schema-surface`: 93 rows.
  - `unsupported-temporal-table-surface`: 43 rows.
  - `big-test-profile`: 6 rows, including 4 selector-expanded `large_tests`
    rows plus two earlier exact probes.
  - `stress-runner-profile`: 8 rows.
  - `mtr-runner-selftest`: 14 rows.
  - `client-utility-profile`: 64 rows.
  - `daemon-utility-profile`: 10 rows.
  - `protocol-profile`: 2 rows.
  - `disabled-event-surface`: 3 rows.
  - `disabled-select-procedure`: 1 row.
  - `disabled-help-surface`: 1 row.
  - `disabled-sequence-runtime`: 1 row.
  - `disabled-flush-surface`: 15 rows.
  - `disabled-table-maintenance`: 4 rows.
  - `debug-only`: 48 rows, including exact main-suite debug probes that source
    debug runtime prerequisites.
  - `disabled-status-metadata`: 11 rows.
  - `disabled-zlib-compression`: 5 rows.
  - `disabled-delayed-insert`: 4 rows.
  - `disabled-json-table-function`: 4 rows.
  - `disabled-sformat-function`: 1 row.
  - `disabled-des-function`: 1 row.
  - `disabled-static-show-info`: 1 row.
  - `packaging-profile`: 1 row.
  - `embedded-skip`: 26 rows.
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
  nor exact `sys_vars` tests for debug-only `new_mode`, native
  `storage_engine`, server-account `read_only`, or slow-launch status
  behavior, nor exact main-suite debug-only and profiling probes,
  nor `engines` stored-procedure, stored-function, trigger, or native InnoDB
  bootstrap tests,
  nor `main.view*` or `main.trigger*` tests,
  nor selected `main.sp*` stored-procedure tests outside the curated accepted
  smoke rows,
  nor `main.partition*` or `engines.tc_partition*` tests, nor `sys_vars` tests
  whose names start with `myisam`, `performance_schema`, `profiling`,
  `query_cache`, `thread_pool`, or `userstat`, nor `main` tests from the
  account, TLS, plugin, backup, query-cache, binlog, replication, file-I/O,
  foreign-server, processlist, UDF, XA, userstat, vector, or GIS families
  listed above, nor `funcs_1` stored-program, trigger, view, processlist,
  privilege metadata, event metadata, or routines metadata families.
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
