# Embedded InnoDB Recovery Start Attribution

## Problem

After clean-shutdown redo-tail truncation removed the repeated warm-open redo
rebuild, production probes still showed visible process-style startup cost in
native InnoDB recovery bootstrap. The existing counters exposed
`recv_recovery_from_checkpoint_start()` as the leading non-rebuild child, but
that bucket still grouped clean redo scanning, optional crash-space discovery,
doublewrite recovery, checkpoint validation, recovered redo setup, file rename
reconciliation, and deferred-space reinitialization.

CI evidence also showed occasional confusion between the compact summary and
the detailed phase output: `trx_lists_init_at_db_start()` could be visible in
the detailed output, but the summary omitted that child, making
`recovery_bootstrap_total` look unexplained. MyLite needs the recurring
startup cost split before deciding whether a native InnoDB fast path is safe.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/srv/srv0start.cc:srv_start()` calls
  `recv_recovery_from_checkpoint_start()`, then `recv_sys.close_files()`,
  insert-buffer checks, `dict_boot()`, `srv_load_tables()`,
  `trx_lists_init_at_db_start()`, and optional binlog offset reporting inside
  the existing recovery bootstrap block.
- `mariadb/storage/innobase/log/log0recv.cc:recv_recovery_from_checkpoint_start()`
  does clean-start work under the redo log latch: capacity setup, an initial
  `recv_scan_log(false, parser)`, a second clean rescan, optional crash-space
  discovery, optional doublewrite recovery, optional final scan, checkpoint
  validation, `log_sys.set_recovered()`, `recv_rename_files()`, and deferred
  tablespace reinitialization.
- `mariadb/storage/innobase/trx/trx0trx.cc:trx_lists_init_at_db_start()`
  creates `purge_sys`, restores the rollback-segment array through
  `trx_rseg_array_init()`, checks whether undo is empty, optionally scans undo
  logs to resurrect active or prepared transactions, and clones the purge view.
- `mariadb/storage/innobase/trx/trx0rseg.cc:trx_rseg_array_init()` iterates
  the fixed rollback-segment slots, restores persistent rollback segments, and
  reads undo-slot metadata through `trx_undo_lists_init()`.

## Design

Extend the disabled-by-default startup counter table with two finer-grained
groups:

- `recv_recovery_from_checkpoint_start()`:
  - `log_sys.set_capacity()`;
  - initial clean redo scan;
  - clean redo rescan;
  - crash-space discovery;
  - missing-tablespace validation scans;
  - doublewrite recovery;
  - final recovery scan;
  - checkpoint validation;
  - recovered redo setup;
  - file rename reconciliation;
  - deferred-space reinitialization.
- `trx_lists_init_at_db_start()`:
  - purge-system creation;
  - rollback-segment array initialization;
  - undo-empty check;
  - recovered transaction scan;
  - purge-view clone;
  - rollback-segment restore time;
  - restored rollback-segment count;
  - restored undo-slot counts by active, prepared, and cached state;
  - resurrected transaction count;
  - recovered table-lock resurrection time.

Expose the dominant `innodb_recovery_trx_lists` child in the compact
`mylite_perf_summary_*` output so CI timing summaries can compare the parent
and child buckets without requiring the full detailed log. The counters observe
existing statements only. They do not skip recovery, reorder redo scanning,
change rollback-segment restoration, change temporary tablespace lifecycle, or
change public API behavior.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, WordPress, or native storage
behavior changes. This slice only adds internal performance counters and probe
output.

## Directory And Lifecycle Impact

No database-directory layout change. InnoDB redo, undo, rollback-segment,
temporary tablespace, and recovered-file lifecycle behavior remains unchanged
inside the existing `mylite_open()` and `mylite_close()` process lifecycle.

## Native Storage Impact

No native InnoDB format or recovery semantics change. The new counters time
existing clean-start and crash-recovery branches without changing their
predicate or order.

## Build And Size Impact

The slice adds counter enum slots and timestamp reads when startup attribution
is enabled. It adds no dependency and no public API.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` and
  `mylite_embedded_open_close_test` with the production embedded PHP preset.
- Run a reduced production performance probe and confirm the new
  `innodb_recovery_start_*` and `innodb_recovery_trx_lists_*` keys are emitted.
- Run focused embedded lifecycle coverage.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output splits `recv_recovery_from_checkpoint_start()` into clean redo
  scan, rescan, and post-scan setup subphases.
- Probe output splits `trx_lists_init_at_db_start()` into rollback-segment
  restore and recovered-transaction work.
- Compact summaries expose `innodb_recovery_trx_lists` and its dominant
  restore children.
- Focused production embedded lifecycle tests pass.
- Documentation records the measured next startup optimization targets without
  claiming a behavior change.

## Verification Results

A reduced five-iteration production probe after adding the counters reported
the ordinary warm-open non-rebuild breakdown:

- `mylite_perf_summary_ordinary_warm_open_close_ms_avg=120.698`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_total_ms_avg=38.910`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_srv_start_recovery_bootstrap_ms_avg=22.632`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_ms_avg=18.930`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_scan_initial_ms_avg=15.194`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_start_scan_rescan_ms_avg=3.724`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_trx_lists_ms_avg=2.931`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_trx_lists_rseg_array_init_ms_avg=2.881`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_recovery_trx_lists_rseg_mem_restore_ms_avg=2.835`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_recovery_trx_lists_rseg_count=640`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_recovery_trx_lists_undo_slot_count=5`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_recovery_trx_lists_undo_active_count=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_recovery_trx_lists_undo_prepared_count=0`;
- `mylite_perf_ordinary_warm_open_close_startup_phase_innodb_recovery_trx_lists_undo_cached_count=5`;
- `mylite_perf_summary_ordinary_warm_open_close_startup_innodb_system_tables_open_tmp_ms_avg=10.001`.

The remaining non-rebuild startup cost is therefore led by clean redo scanning
and InnoDB temporary tablespace opening. Rollback-segment restore is measurable
but smaller in this sample, and it restored fixed startup metadata rather than
active or prepared recovered work.

## Risks And Follow-Up

The likely optimization targets are now narrower but still recovery-sensitive:
reducing clean redo scan/rescan work and reducing temporary tablespace open
cost. Later slices reduced temporary rollback-segment creation and set the
embedded native redo size to `16777216` bytes, reducing clean scan time while
keeping MariaDB recovery and rebuild predicates intact. Any future scan skip or
fast path must still be a separate slice with crash-recovery, clean-shutdown,
read-only, and native file-lifecycle evidence.
