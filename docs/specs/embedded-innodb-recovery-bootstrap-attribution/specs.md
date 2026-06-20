# Embedded InnoDB Recovery Bootstrap Attribution

## Problem

The `srv_start()` and log-rebuild attribution slices narrowed ordinary warm
embedded startup cost to native InnoDB work. In the latest reduced production
sample, steady no-rebuild log cleanup was cheap while warm-open startup still
reported visible time in recovery bootstrap and system-table startup:

- `startup_innodb_srv_start_recovery_bootstrap_ms_avg=26.482`;
- `startup_innodb_srv_start_system_tables_ms_avg=11.689`;
- `startup_innodb_log_rebuild_no_rebuild_delete_log_files_ms_avg=0.384`.

The recovery-bootstrap bucket still groups redo checkpoint scanning,
`recv_sys` cleanup, insert-buffer upgrade checks, dictionary boot, redo apply,
dictionary-table loading, transaction-list initialization, and optional binlog
offset reporting. The system-table bucket groups
`dict_sys.create_or_check_sys_tables()`, temporary tablespace opening, and the
master timer setup. MyLite needs this split before deciding whether a startup
optimization is safe.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/srv/srv0start.cc:srv_start()` existing-database
  startup calls `recv_recovery_from_checkpoint_start()`, `recv_sys.close_files()`,
  `ibuf_upgrade_needed()`, `ibuf_log_rebuild_if_needed()` when needed,
  `dict_boot()`, `trx_rseg_get_n_undo_tablespaces()`, `recv_sys.apply(true)`,
  `srv_load_tables()`, `trx_lists_init_at_db_start()`, and
  `trx_sys_print_mysql_binlog_offset()` before advancing to log rebuild.
- The same function later calls `dict_sys.create_or_check_sys_tables()`,
  `srv_open_tmp_tablespace(create_new_db)`, and
  `srv_start_periodic_timer(srv_master_timer, srv_master_callback, 1000)` inside
  the current `srv_start_system_tables` bucket.
- `mariadb/storage/innobase/log/log0recv.cc:recv_recovery_from_checkpoint_start()`
  performs checkpoint scanning, optional recovery tablespace discovery,
  doublewrite recovery, log validation, and recovery-state setup.

## Design

Extend the disabled-by-default startup counter table with two groups:

- recovery bootstrap:
  - calls and total elapsed time;
  - checkpoint recovery start;
  - `recv_sys.close_files()`;
  - insert-buffer upgrade check or rebuild path;
  - dictionary boot;
  - rollback-segment tablespace discovery;
  - redo apply;
  - `srv_load_tables()`;
  - transaction-list initialization;
  - binlog offset reporting;
- system tables:
  - calls and total elapsed time;
  - `dict_sys.create_or_check_sys_tables()`;
  - `srv_open_tmp_tablespace()`;
  - master timer setup.

Use the existing performance-probe output style. The counters observe the
current flow around existing statements and early returns. They do not skip
recovery work, reorder redo or dictionary operations, change native file
lifecycle, or change any public API behavior.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, WordPress, or native storage
behavior changes. This slice only adds internal performance counters and probe
output.

## Directory And Lifecycle Impact

No directory layout change. Existing InnoDB recovery, dictionary boot,
temporary tablespace, and timer startup behavior remains unchanged inside the
MyLite database directory lifecycle.

## Native Storage Impact

No native InnoDB redo, doublewrite, dictionary, rollback-segment, temporary
tablespace, or recovery ordering change. The counters observe native startup
phases only.

## Build And Size Impact

The slice adds internal counter slots and timestamp reads when startup
attribution is enabled. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced production performance probe and confirm new
  `innodb_recovery_bootstrap_*` and `innodb_system_tables_*` keys are emitted.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output splits recovery bootstrap and system-table startup into the
  documented subphases.
- Existing startup/open/shutdown probe output remains available.
- Focused production embedded lifecycle tests pass.
- Documentation records the measured dominant warm-open recovery/system-table
  subphase and the next optimization target.

## Verification Results

A reduced five-iteration production probe after adding the counters reported one
low-rebuild sample:

- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=118.760`;
- `mylite_perf_summary_ordinary_warm_open_close_start_mysql_server_init_ms_avg=95.526`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_total_ms_avg=45.010`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_recovery_bootstrap_ms_avg=25.896`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_system_tables_ms_avg=12.288`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_log_rebuild_ms_avg=0.444`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_bootstrap_total_ms_avg=25.888`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_ms_avg=21.594`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_load_tables_ms_avg=0.289`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_recovery_trx_lists_ms_avg=3.338`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_system_tables_total_ms_avg=12.288`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_system_tables_open_tmp_ms_avg=12.224`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_system_tables_create_or_check_ms_avg=0.001`.

The remaining measured warm-open InnoDB startup cost is therefore dominated by
checkpoint/recovery startup (`recv_recovery_from_checkpoint_start()`) and
temporary tablespace opening (`srv_open_tmp_tablespace()`) when the redo rebuild
path is idle.

A later rebuilt-binary rerun of the same reduced probe triggered two actual redo
rebuilds across five warm-open iterations and reported:

- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_total_ms_avg=92.264`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_recovery_bootstrap_ms_avg=23.783`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_log_rebuild_ms_avg=52.438`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_system_tables_ms_avg=10.121`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_if_needed_calls=5`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_no_rebuild_calls=3`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_calls=2`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_total_ms_avg=130.372`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_ms_avg=19.767`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_system_tables_open_tmp_ms_avg=10.088`.

The remaining performance work therefore has two separate targets: reduce or
avoid unexpected warm-open redo rebuilds, and then optimize the stable
non-rebuild native startup costs in checkpoint/recovery startup and temporary
tablespace opening.

The follow-up
`docs/specs/embedded-innodb-recovery-start-attribution/specs.md` slice split the
remaining recovery-start bucket. In a reduced five-iteration production sample,
ordinary warm open/close reported `120.698 ms`, with
`startup_innodb_recovery_start_ms_avg=18.930`,
`startup_innodb_recovery_start_scan_initial_ms_avg=15.194`,
`startup_innodb_recovery_start_scan_rescan_ms_avg=3.724`,
`startup_innodb_recovery_trx_lists_ms_avg=2.931`, and
`startup_innodb_system_tables_open_tmp_ms_avg=10.001`. The compact summary now
includes the transaction-list child, but the measured non-rebuild startup target
is clean redo scanning and temporary tablespace opening rather than recovered
transaction resurrection.

Verification commands run for this slice:

- `tools/mariadb-embedded-build build`;
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_embedded_open_close_test
  -j$(nproc)`;
- reduced production `mylite_embedded_performance_probe` with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=5`, `MYLITE_PERF_SELECT_ITERATIONS=1`,
  `MYLITE_PERF_INSERT_ITERATIONS=1`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=0`.
- `ctest --preset php-embedded-prod -R '^libmylite\.embedded-open-close$'
  --output-on-failure`;
- `tools/check-ci-production-builds`;
- `cmake --build --preset format-check-prod`;
- `git diff --check`.

## Risks And Follow-Up

This slice may show that the remaining time is spread across several native
InnoDB phases. Optimizing recovery or dictionary boot must remain a separate
slice with explicit crash-recovery and native-storage evidence.
