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
- `mariadb/mysql-test/suite/funcs_1/t` contains disabled routine, trigger,
  view, and processlist families: `*_storedproc_*` and `storedproc` source
  stored-procedure include files; `*_trig*` covers engine-specific trigger
  cases plus information-schema trigger metadata probes; `*_func_view`,
  `*_views*` cover function-in-view and view runtime plus information-schema
  view metadata probes; and `processlist*` depends on daemon process-list
  metadata, with upstream comments explicitly noting that the no-protocol
  variants do not make sense under embedded server because the processlist is
  empty.
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
  3,937, and unclassified upstream files dropped to 1,551.
- `tools/mylite-mtr-harness list-unsupported` expanded the selector-backed
  categories to concrete rows:
  - `replication-surface`: 844 rows.
  - `disabled-galera-runtime`: 709 rows.
  - `native-innodb-profile`: 690 rows.
  - `disabled-performance-schema`: 496 rows.
  - `disabled-binlog-runtime`: 209 rows.
  - `disabled-partition-engine`: 220 rows.
  - `disabled-oracle-mode`: 89 rows, including 87 selector-expanded `compat`
    rows plus two earlier exact probes.
  - `disabled-native-encryption-profile`: 73 rows.
  - `disabled-file-io`: 26 rows, including selector-expanded `engines.ld_*`,
    `main.loaddata*`, and `main.outfile*` rows plus earlier exact probes.
  - `native-engine-profile`: 50 rows, including selector-expanded native
    Aria, RocksDB temporary-engine, and `sys_vars.myisam*` rows plus earlier
    exact probes.
  - `native-myisam-sysvar`: 15 rows.
  - `disabled-query-cache`: 18 rows.
  - `disabled-thread-pool`: 11 rows.
  - `disabled-statement-profiling`: 2 rows.
  - `disabled-user-statistics`: 3 rows.
  - `disabled-plugin-surface`: 55 rows.
  - `server-account-surface`: 38 rows.
  - `network-tls-surface`: 25 rows.
  - `network-listener-surface`: 7 rows.
  - `server-log-surface`: 9 rows.
  - `external-backup-surface`: 10 rows.
  - `disabled-vector-surface`: 10 rows.
  - `disabled-gis-surface`: 9 rows.
  - `disabled-udf-runtime`: 6 rows.
  - `disabled-xa-runtime`: 4 rows.
  - `foreign-server-metadata`: 2 rows.
  - `disabled-processlist-metadata`: 6 rows, including selector-expanded
    `funcs_1.processlist*` rows plus two earlier exact probes.
  - `disabled-stored-program-runtime`: 19 rows.
  - `disabled-trigger-runtime`: 24 rows, including engine-specific trigger
    rows plus `funcs_1.is_triggers*` metadata rows.
  - `disabled-view-runtime`: 8 rows, including function-in-view, view runtime,
    and `funcs_1.is_views*` metadata rows.
  - `disabled-sys-schema-surface`: 93 rows.
  - `unsupported-temporal-table-surface`: 43 rows.
  - `big-test-profile`: 6 rows, including 4 selector-expanded `large_tests`
    rows plus two earlier exact probes.
  - `stress-runner-profile`: 8 rows.
  - `mtr-runner-selftest`: 7 rows.
  - `debug-only`: 12 rows.
  - `disabled-status-metadata`: 8 rows.
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
  behavior,
  nor `main.partition*` or `engines.tc_partition*` tests, nor `sys_vars` tests
  whose names start with `myisam`, `performance_schema`, `profiling`,
  `query_cache`, `thread_pool`, or `userstat`, nor `main` tests from the
  account, TLS, plugin, backup, query-cache, binlog, replication, file-I/O,
  foreign-server, processlist, UDF, XA, userstat, vector, or GIS families
  listed above, nor `funcs_1` routine, trigger, view, or processlist families.
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
