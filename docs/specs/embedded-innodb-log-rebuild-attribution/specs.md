# Embedded InnoDB Log-Rebuild Attribution

## Problem

The `srv_start()` attribution slice split native InnoDB startup and identified
the warm-open `srv_start_log_rebuild` bucket as the largest measured subphase in
one reduced production sample. That sample reported
`startup_innodb_srv_start_total_ms_avg=82.674`,
`startup_innodb_srv_start_log_rebuild_ms_avg=31.326`,
`startup_innodb_srv_start_recovery_bootstrap_ms_avg=25.988`, and
`startup_innodb_srv_start_system_tables_ms_avg=12.881`.

The name is misleading for ordinary warm opens: `srv_log_rebuild_if_needed()`
can return through a no-rebuild path when redo size, format, and encryption
already match, but that path still calls `delete_log_files()` to remove garbage
`ib_logfile1` through `ib_logfile101`. Before optimizing startup, MyLite needs
to know whether the measured bucket is actual redo rebuild work or repeated
garbage-file cleanup checks.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/srv/srv0start.cc:delete_log_files()` loops over
  suffixes `1..101` and calls `delete_log_file()` for each.
- `create_log_file()` intentionally calls `delete_log_files()` before creating
  `ib_logfile101`, then atomically renames it to `ib_logfile0` through
  `log_t::resize_rename()`.
- `srv_log_rebuild()` calls `srv_prepare_to_delete_redo_log_file()`, closes the
  old redo log, calls `create_log_file(false, lsn)`, and then calls
  `log_sys.resize_rename()`.
- `srv_log_rebuild_if_needed()` returns immediately for force-recovery and
  read-only modes. On the matching-size/format/encryption path, it calls
  `delete_log_files()` and returns `DB_SUCCESS`; otherwise it calls
  `srv_log_rebuild()`.

## Design

Extend the existing internal startup counter table with log-rebuild counters:

- `srv_log_rebuild_if_needed()` calls and total time;
- force-recovery and read-only skip counts;
- no-rebuild path calls and no-rebuild `delete_log_files()` time;
- actual rebuild calls and total time;
- rebuild preparation time through `srv_prepare_to_delete_redo_log_file()`;
- rebuild `create_log_file(false, lsn)` time;
- rebuild `log_sys.resize_rename()` time.

Expose compact summary and detailed phase keys from
`mylite_embedded_performance_probe`. This slice only observes existing startup
paths. It does not skip garbage-file cleanup, change redo file names, change
rebuild ordering, or alter crash recovery.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, WordPress, or native storage
behavior changes. The slice adds disabled-by-default internal performance
counters and probe output only.

## Directory And Lifecycle Impact

No directory layout change. Redo files and temporary `ib_logfile101` keep the
same MariaDB lifecycle.

## Native Storage Impact

No native InnoDB redo, checkpoint, log-file creation, rename, or recovery
ordering change. The counters observe whether startup takes the no-rebuild
cleanup path or actual rebuild path.

## Build And Size Impact

The slice adds a small number of internal counter slots and timestamp reads when
startup attribution is enabled. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced production performance probe and confirm the new
  `innodb_log_rebuild_*` keys are emitted.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output reports no-rebuild cleanup time separately from actual redo
  rebuild time.
- Existing startup/open/shutdown probe output remains available.
- Focused production embedded lifecycle tests pass.
- Documentation records whether the warm-open log bucket is dominated by
  garbage-file cleanup, actual rebuild work, or another phase.

## Verification Results

A reduced five-iteration production probe after adding the counters reported:

- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=157.731`;
- `mylite_perf_summary_ordinary_warm_open_close_start_mysql_server_init_ms_avg=121.393`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_total_ms_avg=71.756`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_log_rebuild_ms_avg=26.442`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_recovery_bootstrap_ms_avg=26.482`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_system_tables_ms_avg=11.689`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_if_needed_calls=5`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_no_rebuild_calls=4`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_no_rebuild_delete_log_files_ms_avg=0.384`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_log_rebuild_calls=1`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_total_ms_avg=130.486`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_create_log_file_ms_avg=99.935`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_log_rebuild_resize_rename_ms_avg=27.111`.

The repeated garbage-file cleanup path is not the steady-state warm-open
bottleneck in this sample. The `srv_start_log_rebuild` average is inflated by
one actual redo rebuild after database creation, while four no-rebuild opens
spent less than half a millisecond each deleting stale redo names. The next
performance target should shift to either first-post-create redo rebuild
avoidance, if creation-heavy workloads matter, or the steady warm-open recovery
bootstrap and InnoDB system-table phases.

The follow-up
`docs/specs/embedded-innodb-recovery-bootstrap-attribution/specs.md` slice
split those warm-open phases and reported
`startup_innodb_recovery_start_ms_avg=21.594` and
`startup_innodb_system_tables_open_tmp_ms_avg=12.224` in a sample where redo
rebuild was idle. A later rerun triggered two actual redo rebuilds and reported
`startup_innodb_log_rebuild_total_ms_avg=130.372`, confirming that rebuild
trigger frequency remains a separate high-impact startup optimization target.
The follow-up reason-attribution slice showed those rebuilds were caused by
redo file-size mismatch only: observed file size averaged `100663304` bytes,
desired file size averaged `100663296` bytes, and observed/desired redo format
matched.

Verification commands run for this slice:

- `tools/mariadb-embedded-build build`;
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_embedded_open_close_test
  -j$(nproc)`;
- reduced production `mylite_embedded_performance_probe` with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=5`, `MYLITE_PERF_SELECT_ITERATIONS=1`,
  `MYLITE_PERF_INSERT_ITERATIONS=1`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=0`.

## Risks And Follow-Up

If no-rebuild `delete_log_files()` owns the bucket, a later optimization can
evaluate a cheaper garbage-log detection path. That optimization must preserve
cleanup of stale `ib_logfile101` and any old multi-file redo leftovers after
unclean shutdown or upgrade paths.
