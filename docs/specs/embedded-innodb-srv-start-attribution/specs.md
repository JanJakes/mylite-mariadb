# Embedded InnoDB `srv_start()` Attribution

## Problem

The embedded startup attribution slice narrowed the remaining ordinary
process-style startup cost to InnoDB `srv_start()`. A reduced production sample
reported `startup_storage_engine_init_innodb_ms_avg=45.038` and
`startup_innodb_init_srv_start_ms_avg=44.983`, but the existing counters still
treat all native InnoDB startup work as one bucket.

That is not enough evidence for an optimization. `srv_start()` covers early
state setup, InnoDB bootstrapping, monitor/tmp files, async IO and core object
creation, checkpoint or new-log checks, system tablespace open, undo
initialization, redo recovery and dictionary bootstrap, doublewrite/undo
reinitialization, recovered transaction cleanup, background task startup,
system tables, temporary tablespace open, buffer-pool load, and crypto thread
startup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc:innodb_init()` already
  attributes InnoDB initialization to parameter setup, PFS registration,
  `srv_sys_space.check_file_spec()`, `srv_start()`, and post-start setup.
- `mariadb/storage/innobase/srv/srv0start.cc:srv_start()` performs the native
  InnoDB startup sequence and has many early-return error paths. Its warm
  reopen path uses `recv_recovery_read_checkpoint()`,
  `srv_sys_space.open_or_create()`, `fil_system.sys_space->open()`,
  `srv_undo_tablespaces_init()`, `recv_recovery_from_checkpoint_start()`,
  `dict_boot()`, `recv_sys.apply()`, `srv_load_tables()`,
  `srv_log_rebuild_if_needed()`, doublewrite and undo tablespace setup,
  recovered DDL/transaction cleanup, monitor/background startup,
  `dict_sys.create_or_check_sys_tables()`, `srv_open_tmp_tablespace()`,
  `buf_load_at_startup()`, and `fil_crypt_threads_init()`.
- The existing startup counter table in
  `mariadb/include/mylite_embedded_startup_perf.h` is disabled by default and
  is only read by `packages/libmylite/tests/embedded_performance_probe.c`.

## Design

Extend the internal startup counter table with `innodb_srv_start` counters:

- call count and total elapsed time for `srv_start()`;
- early setup through performance-stage registration;
- `srv_boot()` and startup logging;
- monitor and misc tmpfile creation;
- async IO, `fil_system`, buffer pool, redo, recovery, lock-system, page-cleaner,
  and temporary-log-crypto object setup;
- checkpoint or new-redo checks;
- system tablespace open;
- undo tablespace initialization;
- new-database bootstrap;
- existing-database recovery and dictionary bootstrap;
- redo log rebuild and system tablespace size adjustment;
- doublewrite, undo tablespace reinitialization, and rollback segment setup;
- recovered-transaction and tablespace load work;
- background monitor, dict-stats, and FTS task startup;
- InnoDB system tables and temporary tablespace open;
- late buffer-pool load and crypt-thread startup.

Use a small local scope timer in `srv0start.cc` so total `srv_start()` time is
recorded even when the function returns through an existing error path. The
phase markers remain simple elapsed-time observations around existing code
blocks; they do not change startup ordering, native storage behavior, recovery
behavior, locks, background tasks, or options.

Expose compact summary keys and detailed phase keys from
`mylite_embedded_performance_probe`, matching the existing startup-attribution
style.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, WordPress, or native storage
format behavior changes. This slice only adds disabled-by-default internal
performance counters and probe output.

## Directory And Lifecycle Impact

No directory layout change. `mylite_open()` and `mylite_close()` continue to use
the same MariaDB embedded lifecycle and database-directory paths.

## Native Storage Impact

No native InnoDB format, checkpoint, redo, undo, recovery, or background-task
ordering change. The counters observe those phases only.

## Build And Size Impact

The slice adds a small number of internal counter slots and one local helper in
an upstream-derived InnoDB file. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` with the production embedded PHP
  preset.
- Run a reduced production performance probe and confirm the new
  `innodb_srv_start_*` keys are emitted.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- The probe emits `startup_innodb_srv_start_total_ms_avg` and detailed
  `startup_phase_innodb_srv_start_*` keys.
- Existing startup, shutdown, open, ownerless, and SQL hot-path probe output
  remains available.
- Focused production embedded lifecycle tests pass.
- Documentation records the dominant `srv_start()` subphase from the measured
  production probe and identifies the next optimization target without claiming
  behavior changes.

## Verification Results

A reduced five-iteration production probe after adding the counters reported:

- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=162.474`;
- `mylite_perf_summary_ordinary_warm_open_close_start_mysql_server_init_ms_avg=129.505`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_init_srv_start_ms_avg=82.676`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_total_ms_avg=82.674`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_log_rebuild_ms_avg=31.326`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_recovery_bootstrap_ms_avg=25.988`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_system_tables_ms_avg=12.881`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_core_systems_ms_avg=8.344`;
- `mylite_perf_summary_ordinary_active_runtime_reconnect_ms_avg=0.695`.

The `srv_start()` cost is spread across native redo/log rebuild, recovery and
dictionary bootstrap, InnoDB system table and temporary tablespace opening, and
core object setup. The next optimization slice should inspect
`srv_log_rebuild_if_needed()` and recovery bootstrap with source-level recovery
evidence before changing startup behavior.

Verification commands run for this slice:

- `tools/mariadb-embedded-build build`;
- `cmake --preset php-embedded-prod`;
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_embedded_open_close_test
  -j$(nproc)`;
- reduced production `mylite_embedded_performance_probe` with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=5`, `MYLITE_PERF_SELECT_ITERATIONS=1`,
  `MYLITE_PERF_INSERT_ITERATIONS=1`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=0`;
- `bash -n tools/wordpress-phpunit-mysqli-mylite
  tools/check-ci-production-builds tools/wordpress-phpunit-append-timing
  tools/wordpress-phpunit-append-timing-test
  tools/wordpress-phpunit-timing-rollup
  tools/wordpress-phpunit-timing-rollup-test`;
- `tools/check-ci-production-builds`;
- `ctest --preset php-embedded-prod -R '^libmylite\.embedded-open-close$'
  --output-on-failure`;
- `cmake --preset prod`;
- `ctest --preset prod -R
  'tools\.(ci-production-builds|wordpress-phpunit-(timing-rollup|append-timing))'
  --output-on-failure`;
- `cmake --build --preset format-check-prod`;
- `git diff --check`.

## Risks And Follow-Up

The counters may show that startup is spread across several native InnoDB
subsystems rather than one simple knob. Any optimization to skip or defer a
subphase must be a separate slice with recovery and compatibility evidence.
