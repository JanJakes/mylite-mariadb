# WordPress PHPUnit Isolated Child Timing CI

## Problem

The WordPress PHPUnit job now splits build, dependency, database preparation,
performance-probe, database tests, process-isolated tests, and non-isolated
tests into visible CI steps. The remaining slow path is the process-isolated
PHPUnit cost model: the parent releases MyLite-backed `wpdb` handles, a child
PHP process starts WordPress and opens the MyLite database, then the parent may
reconnect.

The harness already has lightweight child-process timing counters, but CI keeps
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` globally so the split
process-isolated steps currently report only shell/PHPUnit wall time. That does
not answer the performance question clearly enough when a run is slow.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` injects
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_PROCESS_TIMINGS` into PHPUnit's child process
  runner. When `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES != 0`, it
  records child count, closed `wpdb` count, optional static property scan
  counts, parent lock-release seconds, child runtime seconds, reconnect
  seconds, and per-child averages.
- The same harness already appends those `wordpress_phpunit_child_process_*`
  keys to the timing summary if they appear in PHPUnit output.
- `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0` is independent of child timing.
  Keeping the static scan disabled avoids the reflection-heavy diagnostic path.
- Local process-isolated samples showed the useful attribution shape:
  a one-test `Tests_Formatting_Emoji` sample reported one child, about
  `312 ms` parent lock release, `4050 ms` child runtime, and `134 ms`
  reconnect. The unprofiled sample still spent about `20 s` shell real for one
  test, so the child/runtime cost is present even when the profile output is
  disabled.

## Design

Keep the global WordPress job default:

```yaml
MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES: "0"
MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN: "0"
```

Enable `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` only on:

- `Run WordPress PHPUnit process-isolated deferred-reconnect suite (test only)`;
- `Run WordPress PHPUnit process-isolated eager-reconnect suite (test only)`.

Do not enable the static `wpdb` scan, mysqli profiling, or keepalive for those
steps. This keeps the test behavior and child isolation policy unchanged while
making per-child cost visible in the GitHub step summary.

Extend `tools/check-ci-production-builds` so CI fails if either process-isolated
step stops enabling the lightweight child timing profile.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, storage, or WordPress coverage
changes. This changes CI diagnostics only.

## Performance Impact

The profile adds a few `microtime(true)` calls and counter updates around the
existing parent lock-release, child process, and parent reconnect sections. It
does not perform the reflection-heavy static scan because
`MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN` remains `0`.

The intent is timing visibility, not a throughput optimization. A slow isolated
step should now reveal whether time is in parent lock release, child runtime,
or parent reconnect.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest audit for `tools.ci-production-builds`.
- Run a focused production process-isolated WordPress sample with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` and
  `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0`, verifying
  `wordpress_phpunit_child_process_*` rows are appended to the timing summary.
- Run `git diff --check`.

## Acceptance Criteria

- The two process-isolated CI steps enable
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1`.
- The non-isolated and database PHPUnit steps keep the global disabled default.
- The static `wpdb` scan remains disabled for CI timing runs.
- The production CI audit requires the profile marker on both isolated steps.
- Timing summary rows expose child count, parent lock-release time, child
  runtime, reconnect time, and per-child averages for isolated CI shards.

## Risks And Follow-Up

This slice only improves timing attribution. It does not reduce the dominant
per-child cost. The next optimization target remains the child process's
WordPress/MyLite bootstrap and the full embedded runtime startup/shutdown that
ordinary process isolation requires.
