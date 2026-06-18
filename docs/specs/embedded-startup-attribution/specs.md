# Embedded Startup Attribution

## Problem

After the embedded shutdown retry work, repeated ordinary warm open/close no
longer spends most of its time in InnoDB log-empty retry sleeps. A reduced
production sample reported `close_total_ms_avg=31.365`, while
`open_start_runtime_ms_avg=122.469` and
`start_mysql_server_init_ms_avg=122.311` dominated the process-style lifecycle.

The existing open-phase probe can attribute MyLite-side `start_runtime()` work
to `mysql_server_init()`, but it cannot show whether that startup cost is
client-library setup, embedded SQL-layer bootstrap, plugin initialization,
storage-engine handlerton initialization, or InnoDB startup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:start_runtime()` measures
  `mysql_server_init()` as one broad bucket.
- `mariadb/libmysqld/libmysql.c:mysql_server_init()` runs client setup and
  calls `init_embedded_server()` for embedded builds.
- `mariadb/libmysqld/lib_sql.cc:init_embedded_server()` runs thread and option
  setup, `init_common_variables()`, `init_server_components()`, ACL/grant
  setup, timezone setup, UDF/filter setup, init-file processing, and DDL
  recovery.
- `mariadb/sql/mysqld.cc:init_server_components()` initializes core server
  state, logging, plugin mutexes, plugin initialization, `ha_init()`, default
  engines, transaction-log recovery, DDL recovery, and status variables.
- `mariadb/sql/sql_plugin.cc:plugin_init()` registers built-in plugins,
  initializes mandatory MyISAM first, and initializes remaining plugins.
- `mariadb/sql/handler.cc:ha_initialize_handlerton()` runs storage-engine
  handlerton initialization and already has shutdown-side engine bucketing.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innodb_init()` performs
  InnoDB parameter setup, optional PFS registration, system-space checks,
  `srv_start()`, and post-start handlerton setup.

## Design

Add a disabled-by-default embedded startup performance table in MariaDB, as a
sibling of the existing shutdown performance table:

- `mylite_embedded_startup_perf_set_enabled(int)`;
- `mylite_embedded_startup_perf_reset(void)`;
- `mylite_embedded_startup_perf_read(uint64_t *out_values, size_t value_count)`;
- `mylite_embedded_startup_perf_count()`;
- `mylite_embedded_startup_perf_add_elapsed()`.

Instrument source-level startup boundaries:

- `mysql_server_init()` total, client setup, and embedded-server startup;
- `init_embedded_server()` phases through common variables, server components,
  ACL/grant, timezone, status/UDF/filter setup, init file, and DDL recovery;
- `init_server_components()` broad phases around core setup, logging,
  pre-plugin setup, plugin initialization, `ha_init()`, default engines,
  transaction-log recovery, and final status setup;
- `plugin_init()` setup, built-in registration, MyISAM first initialization,
  remaining plugin initialization, retry, and reap phases;
- `ha_initialize_handlerton()` storage-engine total and engine buckets;
- `innodb_init()` total, parameter setup, PFS registration, system-space file
  check, `srv_start()`, and post-start setup.

Expose compact summary keys in `mylite_embedded_performance_probe` beside the
existing open/shutdown summaries. This keeps CI startup timings visible without
changing public MyLite APIs or adding timing thresholds.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, storage format, or WordPress
behavior changes. The slice adds internal performance counters and probe
output only.

## Directory And Lifecycle Impact

No directory layout or durable-state change. Startup still follows the same
`mylite_open()` and MariaDB embedded lifecycle.

## Native Storage Impact

No native storage format or recovery ordering change. The counters observe
native storage-engine startup, including InnoDB `srv_start()`.

## Build And Size Impact

The slice adds one small disabled-by-default counter table in MariaDB and a
matching probe enum/output block. It adds no dependency and no public header
surface.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced repeated production probe and inspect the new startup counters.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`, `tools/check-ci-production-builds`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output reports `mysql_server_init()` source-level phases and
  storage-engine/InnoDB startup attribution.
- Existing open and shutdown summary keys remain available.
- Focused embedded lifecycle coverage still passes.
- Documentation records the measured dominant startup phase and the next
  optimization target.

## Verification Results

A reduced five-iteration production probe after adding startup attribution
reported:

- `open_start_runtime_ms_avg=94.546`;
- `start_mysql_server_init_ms_avg=94.364`;
- `startup_server_init_embedded_server_ms_avg=94.320`;
- `startup_embedded_server_common_variables_ms_avg=13.074`;
- `startup_embedded_server_components_ms_avg=60.768`;
- `startup_server_components_plugin_init_ms_avg=52.325`;
- `startup_plugin_init_remaining_ms_avg=52.210`;
- `startup_storage_engine_init_innodb_ms_avg=45.038`;
- `startup_storage_engine_init_aria_ms_avg=5.417`;
- `startup_innodb_init_srv_start_ms_avg=44.983`.

The startup bottleneck is therefore native storage-engine startup inside
MariaDB plugin initialization, primarily InnoDB `srv_start()`, with a smaller
fixed contribution from common variable initialization and Aria startup.

## Risks And Follow-Up

The counters may identify startup as mostly native InnoDB `srv_start()` or
plugin initialization. Any optimization that changes startup semantics should
be a separate slice with compatibility and recovery evidence.
