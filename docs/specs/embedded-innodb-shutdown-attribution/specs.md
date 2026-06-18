# Embedded InnoDB Shutdown Attribution

## Problem

Storage-engine shutdown attribution narrowed ordinary warm open/close shutdown
cost to InnoDB handlerton panic shutdown: a reduced production sample reported
`215.685 ms` in the single InnoDB finalizer, almost all of it under
`hton->panic(HA_PANIC_CLOSE)`. The next performance decision needs to know
which InnoDB shutdown phase owns that time.

This slice adds attribution only. It does not skip InnoDB shutdown, alter
`innodb_fast_shutdown`, retain InnoDB state across `mylite_close()`, remove
flush/checkpoint waits, or change crash-recovery semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc` registers
  `innobase_hton->panic = innobase_end`; `innobase_end()` calls
  `innodb_shutdown()` when `srv_was_started`.
- `mariadb/storage/innobase/srv/srv0start.cc` `innodb_shutdown()` calls
  `innodb_preshutdown()`, then for normal/export-restored operation calls
  `logs_empty_and_mark_files_at_shutdown()`, then closes async I/O, file spaces,
  background threads, monitor/temp files, dictionary stats, crypto threads,
  adaptive hash state, log/purge/trx/buffer/doublewrite/lock systems,
  tablespaces, parser/recovery/buffer pool state, temp space, thread pool, and
  startup flags.
- `mariadb/storage/innobase/log/log0log.cc`
  `logs_empty_and_mark_files_at_shutdown()` resets shutdown timers, shuts down
  memory-pressure and dictionary stats helpers, enters cleanup state, may start
  buffer-pool dump, then enters a `loop:` that sleeps for `100000` microseconds
  before checking active transactions, background activity, crypto/page-cleaner
  thread exit, buffer dump/load state, recovered rollback, buffer flush, and
  checkpoint stability. A quiet embedded close may still pay that fixed sleep
  before it can prove the shutdown state is idle.

## Design

Extend the internal `mylite_embedded_shutdown_perf` counter family with InnoDB
shutdown counters:

- `innodb_shutdown()` call count and total time;
- top-level phase time for preshutdown, log-empty/mark-files, async-I/O and
  fil-space close, background thread shutdown, monitor/temp-file close,
  dictionary stats deinit, crypto cleanup, adaptive hash disable, core system
  close, buffer pool close, tablespace/temp-space shutdown, and thread-pool end;
- `logs_empty_and_mark_files_at_shutdown()` call count and total time;
- log-empty subphase time for initial timer/memory/dictionary setup, fixed loop
  sleep, active-transaction/background-thread checks, buffer dump and recovered
  rollback waits, buffer flush, checkpoint stability, and final quiet checks.

The embedded performance probe emits detailed `shutdown_phase_innodb_*` rows and
compact summary rows for the expected dominant buckets:
`innodb_shutdown_total`, `innodb_shutdown_logs_empty`,
`innodb_logs_empty_sleep`, and `innodb_logs_empty_checkpoint`.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, native storage, plugin, or
WordPress application behavior changes. The new counters are internal
instrumentation used by the performance probe.

## Directory And Lifecycle Impact

No directory layout change. `mylite_close()` still follows MariaDB's existing
InnoDB shutdown path and durable checkpoint/flush rules.

## Native Storage Impact

No native storage behavior changes. InnoDB redo, checkpoint, buffer pool,
tablespace, temp-space, and recovery shutdown ordering remain unchanged.

## Build And Size Impact

The slice adds shutdown-only counters and timing calls. It introduces no new
dependency and does not change the embedded plugin profile.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build the production embedded performance probe.
- Run a reduced production embedded performance probe and verify InnoDB
  shutdown and log-empty rows are emitted.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`.
- Run `git diff --check`.

## Verification Results

A reduced production probe with one open/close iteration reported:

- `innodb_shutdown_total_ms=248.695`;
- `innodb_shutdown_preshutdown_ms=6.526`;
- `innodb_shutdown_logs_empty_ms=232.035`;
- `innodb_shutdown_core_close_ms=4.373`;
- `innodb_shutdown_buf_pool_close_ms=1.921`;
- `innodb_shutdown_tablespaces_ms=3.713`;
- `innodb_logs_empty_total_ms=232.034`;
- `innodb_logs_empty_setup_ms=31.297`;
- `innodb_logs_empty_sleep_ms=200.309`;
- `innodb_logs_empty_checkpoint_ms=0.011`.

The dominant ordinary warm open/close shutdown cost therefore belongs to the
fixed sleep in `logs_empty_and_mark_files_at_shutdown()`'s shutdown loop, not to
checkpoint, buffer-pool flush, generic InnoDB close, or MyLite ownerless state.
The next performance slice should evaluate whether an embedded idle close can
prove quiet state before sleeping while preserving MariaDB's checkpoint and
recovery invariants.

## Acceptance Criteria

- `srv0start.cc` and `log0log.cc` compile with the internal shutdown counter
  header.
- Probe output identifies which InnoDB shutdown phase owns the dominant
  ordinary warm open/close shutdown time.
- Probe output identifies whether the dominant log-empty cost is the fixed loop
  sleep, checkpoint work, buffer flush, or another wait.
- Documentation records that this is attribution only and does not claim an
  InnoDB shutdown optimization.

## Risks And Follow-Up

If the fixed shutdown-loop sleep dominates, the next slice should evaluate
whether embedded single-handle idle close can safely check quiet state before
sleeping or otherwise avoid the wait without weakening checkpoint and recovery
guarantees. If checkpoint or buffer flush dominates instead, the next slice
should target that specific InnoDB phase.
