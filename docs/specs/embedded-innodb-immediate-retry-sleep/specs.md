# Embedded InnoDB Immediate Retry Sleep

## Problem

Skipping the first embedded InnoDB log-empty shutdown sleep cut the reduced
ordinary warm open/close sample from `248.695 ms` to `125.473 ms`, but one
`CHECK_INTERVAL` sleep still remained. The post-optimization sample reported
`innodb_logs_empty_sleep_ms=100.470` while checkpoint work was only
`0.017 ms`.

The remaining sleep happens after the immediate first pass enters MariaDB's
retry loop. The retry reason can be transient active-transaction state,
background thread shutdown, or checkpoint LSN movement. Embedded shutdown can
try a bounded set of immediate retries before falling back to MariaDB's fixed
sleep because completion is still gated by the same checks.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/log/log0log.cc`
  `logs_empty_and_mark_files_at_shutdown()` goes to `loop` when active
  transactions remain, when background thread shutdown needs another pass, or
  when `lsn_changed` after `log_make_checkpoint()`. The next loop currently
  pays `CHECK_INTERVAL` before rechecking active transactions, background
  activity, buffer flush, and checkpoint stability.
- The previous embedded first-sleep optimization skips only the first loop
  sleep. It intentionally left retry sleeps unchanged.
- The reduced production probe after that optimization shows checkpoint work is
  effectively zero compared to the remaining retry sleep.

## Design

For `EMBEDDED_LIBRARY` builds only, allow up to 64 immediate retries in
`logs_empty_and_mark_files_at_shutdown()`:

- keep the existing first-sleep skip;
- when active-transaction, background-thread, or checkpoint movement is the
  reason for `goto loop`, set a skip-next-sleep flag while decrementing a
  per-call retry budget;
- consume that skip flag at the top of the next loop;
- stop setting it once the retry budget is exhausted.

If shutdown still needs another retry after the bounded immediate retries, the
loop falls back to MariaDB's existing `CHECK_INTERVAL` sleep. Completion still
requires active transactions, background activity, buffer flush, checkpoint
stability, and final quiet checks to pass.

The non-embedded daemon build keeps MariaDB's original retry sleep behavior.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, plugin, or WordPress application
behavior changes. This changes only embedded InnoDB shutdown latency before
`mylite_close()` returns.

## Directory And Lifecycle Impact

No directory layout change. `mylite_close()` still requires the same InnoDB
quiet-state, flush, checkpoint, and final recovery checks before returning.

## Native Storage Impact

InnoDB native storage ordering remains unchanged. The optimization changes
whether a bounded set of retries wait before rerunning the same checks.

## Build And Size Impact

The slice adds one embedded-only boolean flag and one embedded-only retry-budget
counter in shutdown-only code and no new dependency.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build the production embedded performance probe.
- Run a reduced production embedded performance probe and verify
  `innodb_logs_empty_sleep_ms` drops from the prior `100.470 ms` sample when the
  bounded immediate retry budget is enough.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`.
- Run `git diff --check`.

## Verification Results

A reduced production probe with one open/close iteration after the embedded
immediate-retry budget reported:

- `innodb_shutdown_total_ms=16.315`, down from the prior `125.473` sample;
- `innodb_shutdown_logs_empty_ms=1.569`, down from `101.397`;
- `innodb_logs_empty_sleep_ms=0.000`, down from `100.470`;
- `innodb_logs_empty_background_wait_ms=1.515`;
- `innodb_logs_empty_checkpoint_ms=0.012`.

The optimization removes the remaining fixed `CHECK_INTERVAL` sleep from the
ordinary warm embedded shutdown sample while leaving a finite fallback path for
longer-lived shutdown conditions.

## Acceptance Criteria

- The optimized embedded build still passes the focused lifecycle test.
- Reduced production probe output shows the remaining fixed retry sleep is no
  longer paid for ordinary warm open/close when the bounded immediate retry
  budget reaches stability.
- If checkpoint movement repeats, the code still falls back to the existing
  sleep path instead of busy-spinning.
- Documentation records the optimization as embedded-only and bounded to a
  finite retry budget.

## Risks And Follow-Up

If active transactions, background work, or checkpoint movement exceed the retry
budget in larger workloads, fixed sleeps may remain. Further retry-policy
changes would need broader recovery and shutdown evidence, not just the reduced
probe. The follow-up
`docs/specs/embedded-innodb-retry-attribution/specs.md` slice added repeated
open/close retry-reason counters before narrowing the remaining embedded
background-thread retry sleep.
