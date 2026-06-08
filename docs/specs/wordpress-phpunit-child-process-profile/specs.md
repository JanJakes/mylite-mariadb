# WordPress PHPUnit Child Process Profile

## Problem

The WordPress PHPUnit CI job now separates Docker image build, source fetch,
MyLite PHP extension build, dependency installation, database preparation,
performance probing, and two PHPUnit-only suite partitions. The remaining
suite can still look slow without showing whether the time is ordinary test
execution or PHPUnit's separate-process path.

MyLite's WordPress harness patches PHPUnit's `DefaultPhpProcess` so the parent
process closes MyLite-backed `wpdb` handles before `proc_open()`, allowing the
child process to open the same ordinary MyLite database directory. That lock
release is required for correctness, but it can add visible wall time when a
suite has many process-isolated tests. CI needs parseable timing keys for that
path before tuning the runtime or changing suite policy.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` patches
  `vendor/phpunit/phpunit/src/Util/PHP/DefaultPhpProcess.php` during the
  `dependencies` phase with the
  `MYLITE_WORDPRESS_RELEASE_DB_LOCKS_FOR_CHILD_PROCESSES` marker.
- The patch closes `$GLOBALS['wpdb']` and any static properties that look like
  `wpdb` handles before `proc_open()`, then reconnects closed handles after the
  child process exits.
- The existing WordPress mysqli `perf-probe` already reports stock PHP process
  startup, MyLite-extension process startup, process-plus-connect, in-process
  connect/close, and SQL loop throughput. A current pinned local run reported
  process-plus-connect `633.871ms`, in-process connect/close `441.037ms`,
  `SELECT 1` `248.80 ops/s`, prepared autocommit inserts `325.00 ops/s`, and
  direct autocommit inserts `535.79 ops/s`.
- The pinned `Tests_DB` PHPUnit step remains fast on the current branch: `651`
  tests completed with PHPUnit `Time: 00:15.871` and
  `wordpress_phpunit_seconds=29`.

## Design

Extend the PHPUnit patch with an observability-only timing block guarded by the
new `MYLITE_WORDPRESS_PHPUNIT_CHILD_PROCESS_TIMINGS` marker. The patch records
per-parent totals in `$GLOBALS` and emits them at PHP shutdown when at least one
child process ran:

- `wordpress_phpunit_child_process_count`,
- `wordpress_phpunit_child_process_closed_wpdbs`,
- `wordpress_phpunit_child_process_lock_release_seconds`,
- `wordpress_phpunit_child_process_runtime_seconds`,
- `wordpress_phpunit_child_process_reconnect_seconds`.

The harness forwards `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES`, which
defaults to `1`. Setting it to `0` disables the timing block while preserving
the database lock-release behavior.

The patcher also upgrades existing local/vendor trees that already contain the
older MyLite lock-release patch. If the old block cannot be matched exactly,
the patcher fails during `dependencies` instead of producing a partially timed
PHPUnit file.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, or WordPress test coverage change.
The existing lock-release behavior remains in place.

## Directory And Lifecycle Impact

No durable directory-layout change. The instrumentation only times parent
`wpdb` close/reconnect around PHPUnit child processes.

## Performance Impact

The default CI suite gains five short log lines at parent PHP shutdown when
process-isolated tests run. The instrumentation uses `microtime(true)`, simple
integer/float counters, and one shutdown callback per parent PHPUnit process.
The overhead is negligible compared with the measured MyLite process-plus-
connect and in-process connect/close costs.

The emitted keys let CI distinguish:

- parent lock-release/reflection scan time,
- child process runtime, including PHP startup and test body,
- parent reconnect time after children,
- how many `wpdb` handles were closed for child access.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the `dependencies` phase on a warmed tree and confirm the existing
  PHPUnit patch is upgraded to include
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_PROCESS_TIMINGS`.
- Run a process-isolated WordPress PHPUnit test and confirm the new keys emit.
- Run a non-process-isolated WordPress PHPUnit test and confirm it still passes.
- Run the WordPress mysqli `perf-probe` phase for process/connect and SQL-loop
  context.
- Run the focused `^Tests_DB` WordPress PHPUnit phase.
- Run `ctest --preset php-embedded-dev -L php --output-on-failure`.
- Run `cmake --build --preset format-check`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-08 used the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, the existing Docker image, warmed
build trees, and a WordPress database on host `/tmp` mounted into the
container.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- Current WordPress mysqli `perf-probe` passed with process-plus-connect
  `633.871ms`, in-process connect/close `441.037ms`, `SELECT 1`
  `248.80 ops/s`, transactional prepared inserts `385.42 ops/s`, point
  selects `234.19 ops/s`, prepared autocommit inserts `325.00 ops/s`, and
  direct-string autocommit inserts `535.79 ops/s`.
- The focused `^Tests_DB` PHPUnit phase passed `651` tests with `3` skips and
  reported PHPUnit `Time: 00:15.871`, `wordpress_phpunit_shell_real_seconds=28.798`,
  and `wordpress_phpunit_seconds=29`.
- An uninstrumented `^(?!Tests_DB)` remaining-suite run reached the PHPUnit
  summary with `28597` tests, `3439345` assertions, `86` warnings, and `91`
  skips. PHPUnit reported `Time: 58:57.702`,
  `wordpress_phpunit_shell_real_seconds=3551.177`,
  `wordpress_phpunit_shell_user_seconds=1682.231`,
  `wordpress_phpunit_shell_sys_seconds=1595.925`, and
  `wordpress_phpunit_seconds=3551`. The outer wrapper then hit a shell
  read-offset artifact because the harness file was edited while that command
  was still running, so this run is recorded as performance evidence rather
  than a clean command pass.
- `MYLITE_WORDPRESS_PHASE=dependencies` upgraded the existing local PHPUnit
  `DefaultPhpProcess.php` from the old MyLite lock-release marker to the new
  timing marker and completed with `wordpress_total_seconds=5` on the warmed
  tree.
- A follow-up `MYLITE_WORDPRESS_PHASE=dependencies` run against the fully
  upgraded vendor file completed with `wordpress_phpunit_patch_seconds=0`,
  confirming the patch is idempotent after both runtime and reconnect
  accumulators are present.
- A focused process-isolated test,
  `^Tests_Formatting_Emoji::test_print_emoji_detection_script_on_front_end$`,
  passed and emitted:
  `wordpress_phpunit_child_process_count=1`,
  `wordpress_phpunit_child_process_closed_wpdbs=1`,
  `wordpress_phpunit_child_process_lock_release_seconds=0.354302`,
  `wordpress_phpunit_child_process_runtime_seconds=5.809931`, and
  `wordpress_phpunit_child_process_reconnect_seconds=0.122135`.
- The same focused process-isolated test passed with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` and emitted no
  `wordpress_phpunit_child_process_*` keys, confirming the opt-out disables
  only timing output.
- A focused non-process-isolated test,
  `^(?!Tests_DB).*Tests_Actions::test_simple_action$`, passed and emitted no
  child-process keys.
- `build/php-embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed on the loaded host with ordinary warm open/close `705.112ms`,
  ownerless warm open/close `779.555ms`, ordinary direct `SELECT 1`
  `2217.27 ops/s`, ordinary autocommit inserts `885.95 ops/s`, ownerless
  direct `SELECT 1` `1583.35 ops/s`, and ownerless autocommit inserts
  `99.07 ops/s`.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed and showed the ownerless autocommit insert cost remained concentrated
  in ownerless-only native visibility work: page-publish hook total
  `199.051ms` with append `178.736ms`, page-read total `249.523ms`,
  page-write refresh `216.781ms`, page-write publish `226.851ms`, and
  page-log append total `178.249ms` for `200` ownerless autocommit inserts.
- `ctest --preset php-embedded-dev -L php --output-on-failure` passed `3`
  tests in `12.32 sec`.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Existing split CI build/setup/prepare/perf/phpunit steps remain unchanged.
- The PHPUnit `dependencies` phase can upgrade an existing old MyLite
  `DefaultPhpProcess` patch and can patch a clean PHPUnit vendor file.
- Process-isolated PHPUnit executions print child-process count, lock-release,
  child-runtime, reconnect, and closed-handle totals.
- `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` disables only timing
  output, not the required MyLite DB lock-release patch.
