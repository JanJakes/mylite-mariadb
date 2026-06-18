# Embedded InnoDB Retry Attribution

## Problem

The embedded-only immediate-retry budget removed the fixed InnoDB log-empty
shutdown sleep from a one-iteration ordinary warm open/close sample, but a
ten-iteration reduced production probe still averaged about `90 ms` in
`innodb_logs_empty_sleep_ms_avg`. That means the first sample proved the fast
path can work, while repeated process-style open/close still sometimes exhausts
the retry budget and falls back to MariaDB's `CHECK_INTERVAL` sleep.

The existing shutdown counters time the loop and checkpoint work but do not
report loop counts, skip counts, retry reasons, or the reason that led to an
actual sleep. Without that evidence, increasing the retry budget or changing
background-thread shutdown policy would be a guess.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/log/log0log.cc`
  `logs_empty_and_mark_files_at_shutdown()` loops after active transactions,
  background thread shutdown, or checkpoint LSN movement. The existing MyLite
  embedded-only path skips the first sleep and grants up to 64 immediate retry
  sleeps before falling back to MariaDB's fixed `CHECK_INTERVAL`.
- `mariadb/include/mylite_embedded_shutdown_perf.h` and
  `packages/libmylite/tests/embedded_performance_probe.c` already carry
  matching shutdown stat indexes and probe output for shutdown phase timing.
- A repeated reduced production sample after the immediate-retry slice still
  showed `innodb_shutdown_logs_empty_ms_avg=94.418` and
  `innodb_logs_empty_sleep_ms_avg=90.193`, so the next bottleneck is budget
  exhaustion frequency and cause, not the checkpoint timer itself.

## Design

Add shutdown attribution counters for `EMBEDDED_LIBRARY` builds and expose them
through the production performance probe:

- log-empty loop iterations;
- actual sleep calls and skipped sleep calls;
- immediate-retry skip grants and budget exhaustion;
- active-transaction, background-thread, and checkpoint retry counts;
- actual sleeps attributed to the previous retry reason.

Use those counters to keep the policy change narrow. If the actual sleep
follows a background-thread retry, embedded shutdown uses a `1 ms` poll
interval after the immediate-retry budget is exhausted instead of MariaDB's
daemon-oriented `100 ms` `CHECK_INTERVAL`. Active-transaction and checkpoint
retry sleeps keep the existing MariaDB interval.

The completion gates are unchanged: shutdown still requires active
transactions, background activity, buffer flush, checkpoint stability, and
final quiet checks to pass.

## Compatibility Impact

No SQL, C API, PHP mysqli API, wire-protocol, storage format, or WordPress
behavior changes. The slice adds performance counters to embedded shutdown
instrumentation and shortens only the embedded background-thread retry poll.

## Directory And Lifecycle Impact

No directory layout or durable-state change. `mylite_close()` still waits for
the same InnoDB active-transaction, background-thread, buffer-flush,
checkpoint, and final quiet-state checks.

## Native Storage Impact

No native InnoDB ordering change. The counters observe the existing MariaDB
shutdown loop and MyLite's embedded-only immediate-retry budget.

## Build And Size Impact

The slice adds a small set of stat indexes and counter increments in
shutdown-only code plus one embedded-only background retry sleep interval. It
adds no dependency and no default runtime work when the performance probe has
shutdown stats disabled.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced repeated production probe and inspect the new retry counters.
- Verify the repeated probe no longer pays the `100 ms` retry sleep when the
  remaining retry reason is only background-thread shutdown.
- Run `libmylite.embedded-open-close`.
- Run `format-check-prod`, `tools/check-ci-production-builds`, and
  `git diff --check`.

## Acceptance Criteria

- Probe output reports log-empty loop counts, sleep counts, skip counts,
  retry-reason counts, and reason-attributed sleep counts.
- Repeated ordinary warm open/close reduces InnoDB log-empty retry sleep from
  the prior `100 ms` cadence to the embedded background poll interval when all
  actual sleeps are background-thread sleeps.
- The focused embedded lifecycle test still passes.
- Documentation records that repeated open/close still needed attribution
  before a broader retry-budget or background-thread policy change.

## Verification Results

A reduced ten-iteration production probe before the background-poll change
reported:

- `innodb_logs_empty_sleep_calls=9`;
- `innodb_logs_empty_retry_skip_budget_exhausted=9`;
- `innodb_logs_empty_background_retries=586`;
- `innodb_logs_empty_sleep_after_background=9`;
- no active-transaction or checkpoint retries;
- `innodb_logs_empty_sleep_ms=902.541`;
- `innodb_logs_empty_total_ms=974.099`.

The same reduced ten-iteration production probe after the embedded background
poll change reported:

- `innodb_logs_empty_sleep_calls=10`;
- `innodb_logs_empty_retry_skip_budget_exhausted=10`;
- `innodb_logs_empty_background_retries=650`;
- `innodb_logs_empty_sleep_after_background=10`;
- no active-transaction or checkpoint retries;
- `innodb_logs_empty_sleep_ms=11.280`;
- `innodb_logs_empty_total_ms=54.854`.

An immediately repeated summary sample reported
`ordinary_warm_open_close_ms_avg=156.240`, with
`open_start_runtime_ms_avg=122.469`, `close_total_ms_avg=31.365`,
`innodb_shutdown_total_ms_avg=28.594`, and
`innodb_logs_empty_sleep_ms_avg=0.731`. The remaining dominant repeated
process-style cost is startup/open, not the InnoDB log-empty retry sleep.

## Risks And Follow-Up

The `1 ms` background poll is still a poll. If a workload repeatedly exhausts
the immediate retry budget for active transactions or checkpoint movement, this
slice intentionally leaves MariaDB's longer sleep in place until there is
separate evidence for those paths. The next performance slice should focus on
`mysql_server_init()` startup attribution and trunk comparison.
