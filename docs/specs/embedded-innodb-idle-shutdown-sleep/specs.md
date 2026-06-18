# Embedded InnoDB Idle Shutdown Sleep

## Problem

InnoDB shutdown attribution showed that ordinary warm embedded open/close spends
most shutdown time in `logs_empty_and_mark_files_at_shutdown()`'s fixed loop
sleep. A reduced production probe reported `248.695 ms` in
`innodb_shutdown()`, `232.035 ms` in log-empty shutdown, and `200.309 ms` in the
loop sleep while checkpoint work was `0.011 ms`.

The current MariaDB loop sleeps for `100000` microseconds before it checks
whether shutdown is already quiet. That is conservative for daemon shutdown,
but it is expensive for MyLite's short-lived embedded/process-isolated test
children where the first check can often prove no active transactions,
background activity, or checkpoint work remains.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/log/log0log.cc`
  `logs_empty_and_mark_files_at_shutdown()` enters `loop:`, sleeps for
  `CHECK_INTERVAL` (`100000` microseconds), then checks active transactions,
  background thread state, buffer dump/load and recovered rollback state,
  buffer-pool flush, checkpoint stability, and final quiet assertions.
- The previous attribution slice records the loop sleep separately from buffer
  flush and checkpoint work. In the reduced embedded sample, checkpoint work was
  effectively zero compared to fixed sleep.
- `mariadb/storage/innobase/srv/srv0start.cc` calls
  `logs_empty_and_mark_files_at_shutdown()` from `innodb_shutdown()` for normal
  and export-restored operation.

## Design

For `EMBEDDED_LIBRARY` builds only, skip the first `CHECK_INTERVAL` sleep in
`logs_empty_and_mark_files_at_shutdown()`. The first loop iteration immediately
runs MariaDB's existing checks:

- active transaction detection still loops if any transaction remains;
- crypto/page-cleaner background waits still loop;
- buffer dump/load and recovered rollback waits still run;
- buffer-pool flush still runs;
- checkpoint stability still loops if the current LSN is not at the checkpoint;
- final quiet assertions still run before recording shutdown LSN.

Any loop retry after the first iteration keeps the existing `100000`
microsecond sleep. This avoids the idle embedded first-sleep penalty without
busy-spinning when there is actual shutdown work or checkpoint movement.

The non-embedded daemon build keeps MariaDB's original first-sleep behavior.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, plugin, or WordPress application
behavior changes. This changes only embedded InnoDB shutdown latency before
`mylite_close()` returns.

## Directory And Lifecycle Impact

No directory layout change. `mylite_close()` still runs InnoDB's existing
shutdown checks, buffer flush, checkpoint stability loop, and final recovery
state publication.

## Native Storage Impact

InnoDB native storage ordering remains unchanged. The optimization changes when
the first quiet-state check happens, not what must be true before shutdown
completes.

## Build And Size Impact

The slice adds a small embedded-only branch in shutdown-only code and no new
dependency.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build the production embedded performance probe.
- Run a reduced production embedded performance probe and verify
  `innodb_logs_empty_sleep_ms` drops from the prior `200.309 ms` sample while
  checkpoint and final close still complete.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`.
- Run `git diff --check`.

## Verification Results

A reduced production probe with one open/close iteration after the embedded
first-sleep skip reported:

- `innodb_shutdown_total_ms=125.473`, down from the prior `248.695` sample;
- `innodb_shutdown_logs_empty_ms=101.397`, down from `232.035`;
- `innodb_logs_empty_sleep_ms=100.470`, down from `200.309`;
- `innodb_logs_empty_checkpoint_ms=0.017`, still effectively zero.

The optimization removes one fixed `CHECK_INTERVAL` sleep from ordinary warm
embedded shutdown. One `100 ms` retry sleep remains in this sample, consistent
with the documented follow-up: the first immediate pass can still loop after
checkpoint LSN movement, and that retry path intentionally keeps MariaDB's
existing sleep behavior in this slice.

## Acceptance Criteria

- The optimized embedded build still passes the focused lifecycle test.
- Reduced production probe output shows the first fixed sleep is no longer paid
  on ordinary warm open/close when shutdown can prove quiet state.
- The retry path still records sleep time if checkpoint movement or background
  work forces another loop.
- Documentation records the optimization as embedded-only and bounded to the
  first sleep.

## Risks And Follow-Up

If the first immediate check often observes checkpoint movement, one
`CHECK_INTERVAL` sleep may remain. A later slice can evaluate whether the
checkpoint-movement retry can also avoid fixed sleep when all active/background
checks already passed, but that needs separate evidence because it changes a
different wait point.
