# WordPress PHPUnit Perf Probe Controls

## Problem

The WordPress PHPUnit harness exposes a `perf-probe` phase to separate PHP
process startup, process plus MyLite connect/close startup, and steady mysqli
SQL loop throughput. The host wrapper documents
`MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS`,
`MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS`,
`MYLITE_WORDPRESS_PERF_SQL_ITERATIONS`, and
`MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS`, but a local audit showed the first
version did not forward those variables into the Docker container. As a result,
callers asking for longer probes still received the default 5 process
iterations, 5,000 read iterations, and 1,000 write iterations.

A follow-up audit found that the host and container both accepted and printed
`MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS`, but the process-plus-MyLite
connect/close loop still reused `MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS`.
That made the per-process startup cost sample less controllable than the
in-process connect/reconnect and SQL/write samples.

## Source Findings

- `tools/wordpress-phpunit-mysqli-mylite` runs the WordPress harness in a
  Docker container and explicitly forwards selected `MYLITE_WORDPRESS_*`
  variables with `-e`.
- The container-side `perf-probe` block already validates positive integer
  iteration counts and emits the selected values before running the probe.
- The host-side Docker invocation did not forward the perf iteration variables,
  so the container always used its fallback defaults.
- The later container-side process-plus-connect/close loop used
  `perf_process_iterations` for both its loop bound and average denominator
  even though `perf_connect_iterations` was separately parsed, forwarded, and
  printed.
- The WordPress mysqli extension opens normal MyLite read/write databases with
  `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE`; this probe measures ordinary
  embedded mysqli runtime, not ownerless read/write mode.

## Design

Forward the perf iteration environment variables through `docker run`. Use the
same default values as the container-side fallback so existing callers, CI
phases, and local default runs keep their current behavior. Invalid explicit
values continue to fail inside the existing container-side validation.

Use `perf_connect_iterations` for the process-plus-MyLite connect/close loop
and denominator. This keeps `wordpress_perf_php_connect_process_ms_avg` and the
matching `wordpress_perf_summary_php_process_connect_close_*` keys aligned with
the printed connect iteration count. Emit
`wordpress_perf_php_connect_process_iterations` and
`wordpress_perf_summary_php_process_connect_close_iterations` next to those
averages so logs identify the sample size used by the startup measurement
itself.

No SQL behavior, PHP API behavior, database lifecycle behavior, or CI default
suite coverage changes.

## Compatibility Impact

The change is observability-only. The full WordPress PHPUnit suite and the
default one-shot harness still run the same WordPress checkout, PHP wrapper,
extensions, and MyLite database directory setup.

## Directory And Lifecycle Impact

No durable files or directory layout change. `perf-probe` continues to reuse
the prepared MyLite database directory and removes only its own temporary
`mylite_perf_probe` table.

## Build And Performance Impact

Longer local probes can now reduce noise when distinguishing per-process
startup cost from in-process engine throughput. The connect-iteration control
now covers both the separate PHP-process connect/close sample and the in-process
connect/reconnect sample. The write-iteration control covers both transactional
and autocommit insert loops in the WordPress `perf-probe`. CI now runs the
probe with bounded iteration counts, while local callers can raise the controls
when they need lower-noise measurements.

A patched detached `/tmp` ownerless worktree confirmed that host overrides were
forwarded into the container: `MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS=10`,
`MYLITE_WORDPRESS_PERF_SQL_ITERATIONS=10000`, and
`MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS=2000` emitted the matching
`wordpress_perf_*_iterations` values. Under the current host load that run
reported PHP process startup `94.331ms`, process plus MyLite connect/close
`555.605ms`, `SELECT 1` `196.21 ops/s`, transactional inserts `313.43 ops/s`,
and primary-key point selects `195.97 ops/s`. The split `Tests_DB` PHP phase
then completed with PHPUnit `00:21.784` and `wordpress_phpunit_seconds=34`,
while a same-machine main `4760d512` one-shot harness run reported PHPUnit
`00:26.911` and `wordpress_phpunit_seconds=44` after spending `132s` in the old
forced build/configure bucket.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the `perf-probe` phase with non-default iteration counts and verify the
  emitted `wordpress_perf_*_iterations` values match the host environment.
- Run a non-default `perf-probe` with different process and connect iteration
  counts and verify the process-plus-connect/close loop reports and uses the
  connect count.
- Run the pinned WordPress `Tests_DB` path through the split `phpunit` phase to
  confirm the suite execution remains isolated from setup/build phases.
- Run `git diff --check`.

## Acceptance Criteria

- Host-provided perf iteration counts are visible inside the container.
- `MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS` controls both process-plus-connect
  and in-process connect/reconnect sample sizes, with dedicated output keys for
  the process-plus-connect/close sample.
- Default `perf-probe` behavior remains unchanged when no overrides are set.
- The split CI build/setup/prepare/phpunit phases are unchanged.
