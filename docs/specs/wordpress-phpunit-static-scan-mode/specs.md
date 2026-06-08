# WordPress PHPUnit Static Scan Mode

## Problem

The WordPress PHPUnit CI job is now split into production-build phases and
prints process-isolated child-process timings. One remaining parent-side cost
inside each process-isolated test is the defensive static-property scan used to
find additional `wpdb` handles before PHPUnit launches a child process.

A focused production sample on this branch closed exactly one handle, the
global WordPress `wpdb`, but still spent `265.969 ms` in parent lock release
before the child process. That scan is useful as a local defensive fallback,
but CI can measure the known WordPress global-`wpdb` process-isolated path more
directly by avoiding static-property probing.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- WordPress ref used by CI: `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `vendor/phpunit/phpunit/src/Util/PHP/DefaultPhpProcess.php` so the parent
  closes MyLite-backed `wpdb` handles before `proc_open()` and reconnects them
  after the child exits.
- WordPress `src/wp-includes/class-wpdb.php` opens a fresh mysqli object in
  `wpdb::db_connect()` and closes the current mysqli object in `wpdb::close()`.
  The global `$GLOBALS['wpdb']` is therefore the ordinary handle that blocks a
  process-isolated child from opening the same MyLite database directory.
- PHP object enumeration is not available in the current local runtimes:
  host PHP `8.4.21` and Docker PHP `8.3.31` both reported
  `function_exists("gc_get_objects") === false`. The slice cannot replace
  reflection with object enumeration.
- Guarded production focused sample before this slice:
  `wordpress_phpunit_child_process_count=1`,
  `wordpress_phpunit_child_process_closed_wpdbs=1`,
  `wordpress_phpunit_child_process_lock_release_ms_avg=265.969`,
  `wordpress_phpunit_child_process_runtime_ms_avg=4096.512`,
  `wordpress_phpunit_child_process_reconnect_ms_avg=103.573`,
  and `wordpress_phpunit_shell_real_seconds=27.635`.

## Design

Add `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN`, defaulting to `1`, to the
WordPress PHPUnit harness. The harness rejects values other than `0` or `1`
before starting Docker and forwards the selected mode into the container.

The injected PHPUnit patch always closes `$GLOBALS['wpdb']` before
`proc_open()`. When `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=1`, it also runs
the existing cached, type-filtered static-property scan. When the value is `0`,
it skips only that static scan.

The CI WordPress job sets `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0` so the
timed process-isolated suite measures the known WordPress global-`wpdb` path.
Local runs and debugging keep the current full scan by default.

The follow-up child-profile mode keeps local child-process timing diagnostics
enabled by default but sets
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` in CI, so test-only
process-isolated timings avoid both static-scan overhead and child-profiling
overhead.

The patcher adds `MYLITE_WORDPRESS_STATIC_WPDB_SCAN_GUARD` so warmed cached
PHPUnit vendor trees are upgraded in place.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, or native storage behavior changes.
This only changes the WordPress PHPUnit harness. The production CI run remains
Release for first-party PHP-extension code and MinSizeRel for the MariaDB
embedded archive.

## Directory And Lifecycle Impact

No durable directory-layout changes. The parent still closes the WordPress
global `wpdb` before child `proc_open()`, releasing the MyLite database
directory lock for process-isolated children.

## Build And Performance Impact

CI avoids rereading thousands of cached static `ReflectionProperty` objects
before every process-isolated child. Full static scan mode remains available
for local defensive runs by setting `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=1`
or leaving the variable unset.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Verify invalid `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN` values are
  rejected before Docker starts.
- Run the production `dependencies` phase on the warmed PHPUnit vendor tree and
  confirm `MYLITE_WORDPRESS_STATIC_WPDB_SCAN_GUARD` is installed.
- Run a focused production process-isolated PHPUnit test with
  `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0` and confirm it passes.
- Run the same focused test with the default static scan enabled to confirm the
  defensive mode still works.
- Run production WordPress `perf-probe` with CI-sized iteration counts for
  ordinary mysqli context.
- Run focused production `^Tests_DB` as the non-process-isolated database
  suite smoke.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- CI explicitly opts into global-`wpdb` fast mode for the WordPress PHPUnit
  timing job while retaining production build guards.
- Default local harness behavior keeps the full static-property scan.
- Warmed PHPUnit vendor trees upgrade without deleting dependencies.
- Focused process-isolated tests pass in both fast and full static-scan modes.

## Verification Results

Local verification on 2026-06-09 used the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, the warmed WordPress Docker image,
the guarded `build/wordpress-php-embedded-prod` Release CMake build, the
guarded `build/wordpress-mariadb-embedded` MinSizeRel MariaDB embedded archive,
and a WordPress MyLite test database mounted from host `/tmp`.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=bad
  tools/wordpress-phpunit-mysqli-mylite` rejected the invalid value before
  Docker startup.
- Guarded production `MYLITE_WORDPRESS_PHASE=dependencies` passed on the
  warmed vendor tree and installed
  `MYLITE_WORDPRESS_STATIC_WPDB_SCAN_GUARD` in PHPUnit's
  `DefaultPhpProcess.php`.
- Focused production `Tests_Formatting_Emoji::test_print_emoji_detection_script_on_front_end`
  passed in default mode with `wordpress_phpunit_static_wpdb_scan=1`,
  `wordpress_phpunit_child_process_count=1`,
  `wordpress_phpunit_child_process_lock_release_ms_avg=255.580`,
  `wordpress_phpunit_child_process_runtime_ms_avg=5737.599`,
  `wordpress_phpunit_child_process_reconnect_ms_avg=102.136`, and
  `wordpress_phpunit_shell_real_seconds=18.772`.
- Production `Tests_Formatting_Emoji` passed in CI fast mode with
  `wordpress_phpunit_static_wpdb_scan=0`,
  `wordpress_phpunit_child_process_count=4`,
  `wordpress_phpunit_child_process_closed_wpdbs=4`,
  `wordpress_phpunit_child_process_lock_release_ms_avg=254.626`,
  `wordpress_phpunit_child_process_runtime_ms_avg=4218.767`,
  `wordpress_phpunit_child_process_reconnect_ms_avg=109.529`, and
  `wordpress_phpunit_shell_real_seconds=31.599`.
- The same production `Tests_Formatting_Emoji` class passed in default full
  static-scan mode with `wordpress_phpunit_static_wpdb_scan=1`,
  `wordpress_phpunit_child_process_count=4`,
  `wordpress_phpunit_child_process_closed_wpdbs=4`,
  `wordpress_phpunit_child_process_lock_release_ms_avg=373.179`,
  `wordpress_phpunit_child_process_runtime_ms_avg=5937.084`,
  `wordpress_phpunit_child_process_reconnect_ms_avg=159.053`, and
  `wordpress_phpunit_shell_real_seconds=41.816`.
- Production WordPress mysqli `perf-probe` with CI-sized iteration counts
  passed and reported process-plus-connect `526.539 ms`, in-process
  connect/close `399.931 ms`, active-runtime reconnect `3.343 ms`, `SELECT 1`
  `238.69 ops/s`, prepared autocommit inserts `327.12 ops/s`, and direct
  autocommit inserts `747.36 ops/s`.
- Production `^Tests_DB` passed `651` tests with `3` skips; PHPUnit reported
  `Time: 00:19.635`, `wordpress_phpunit_shell_real_seconds=32.662`, and
  `wordpress_phpunit_seconds=32`.

## Rejected Alternatives

Deferring parent reconnect after each child was tested and rejected. With
`Tests_Formatting_Emoji`, the first process-isolated children passed, but later
parent-side tests in the same class failed because WordPress called escaping
code while `wpdb` was still disconnected. The failed run produced `15` errors,
`wordpress_phpunit_child_process_closed_wpdbs=1`, and
`wordpress_phpunit_child_process_reconnect_ms_avg=0.009`. The CI filter
includes full classes, so eager reconnect remains required.
