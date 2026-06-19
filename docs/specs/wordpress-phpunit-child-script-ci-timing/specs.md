# WordPress PHPUnit Child Script CI Timing

## Problem

The split WordPress PHPUnit CI job now reports child count, parent lock
release, child-process runtime, reconnect, and baseline-restore timings for
process-isolated shards. That still leaves the main process-isolated bucket too
coarse: `wordpress_phpunit_child_process_runtime_seconds` mixes PHP process
startup, PHPUnit child bootstrap, WordPress bootstrap, MyLite child open/close,
SQL work, and test body runtime.

The harness already has child-script timing, but it is tied to
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1`, which CI intentionally
forbids in timing-sensitive shards. CI needs child script versus outer process
attribution without enabling the full child profiler or mysqli child
aggregation.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `DefaultPhpProcess.php` during the WordPress `dependencies` phase.
- The patched `DefaultPhpProcess::runJob()` can inject a small shutdown timer
  into the generated child PHP job after the opening `<?php` tag.
- The patched `runProcess()` can create a per-child temp file, pass it as
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_FILE`, read it after
  `proc_close()`, and aggregate `wordpress_phpunit_child_script_*` rows without
  writing child timing to stderr, which PHPUnit treats as a test error.
- The existing child timing summary flag creates parent-side stats when
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`.
- The production workflow already sets
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` globally and the
  production audit forbids the process-isolated shards from overriding it to
  `1`.

## Design

Add a narrower harness flag:

`MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_SUMMARY=1`

When either the full child profiler or this new flag is enabled, the patched
PHPUnit runner:

- wraps the generated child PHP job with the existing shutdown timer,
- creates and forwards one temp timing file per child when parent child stats
  are active,
- reads and removes that file after `proc_close()`,
- emits `wordpress_phpunit_child_script_count`,
  `wordpress_phpunit_child_script_seconds`,
  `wordpress_phpunit_child_process_outer_minus_script_seconds`,
  `wordpress_phpunit_child_script_ms_avg`, and
  `wordpress_phpunit_child_process_outer_minus_script_ms_avg`.

Keep `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`,
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=0`, and
`MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0` in CI timing shards. The new flag
does not enable mysqli profile context, static `wpdb` reflection scanning, or
the broader diagnostic child profile mode.

The script-level flag requires either `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`
or full child profiling, because the parent needs child-process stats to report
the script totals and outer-minus-script calculation.

## Affected Subsystems

- WordPress PHPUnit harness script.
- GitHub Actions WordPress PHPUnit job.
- CI production-build/timing audit.
- Performance and compatibility documentation.

## Compatibility Impact

No SQL, PHP API, mysqli API, C API, storage-engine, WordPress test selection, or
ownerless concurrency behavior changes. This is diagnostic output only.

## Directory And Lifecycle Impact

No durable layout changes. The child timing file is a temporary per-child file
under the container temp directory and is removed by the parent after the child
finishes.

## Native Storage Impact

No native storage format, redo, checkpoint, or recovery changes.

## Build, Size, License, And Dependency Impact

No compiled-code, binary-size, dependency, or license impact. CI executes the
same test shards against the same production builds. The added overhead is one
child-side `microtime(true)` shutdown measurement and one small temp-file
read/write per process-isolated child.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest wrapper for `tools.ci-production-builds`.
- Run the WordPress `dependencies` phase against the warmed PHPUnit vendor tree
  and verify `DefaultPhpProcess.php` contains
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_SUMMARY`.
- Run a focused production process-isolated PHPUnit smoke with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`, and
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_SUMMARY=1`, verifying the
  child-script and outer-minus-script rows appear.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-20 used the cached CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, production
`build/wordpress-php-embedded-prod` Release MyLite/PHP artifacts, the
`build/wordpress-mariadb-embedded` MinSizeRel MariaDB embedded archive, and the
default external tmpfs WordPress MyLite database path.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `bash -n tools/check-ci-production-builds` passed.
- `tools/check-ci-production-builds` passed.
- The WordPress `dependencies` phase passed against the warmed PHPUnit vendor
  tree and installed `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_SUMMARY`
  into `DefaultPhpProcess.php`. A repeat dependency phase also passed,
  proving the warmed vendor patch remains idempotent.
- `php -l
  build/wordpress-phpunit-tools/vendor/phpunit/phpunit/src/Util/PHP/DefaultPhpProcess.php`
  passed after the generated patch.
- A focused production process-isolated PHPUnit smoke passed with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_SUMMARY=1`,
  `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`, and
  `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`, using filter
  `Tests_Functions_WpUniquePrefixedId::test_should_create_unique_prefixed_ids`.
  It ran `7` tests with `14` assertions and reported
  `wordpress_phpunit_child_process_runtime_seconds=6.916666`,
  `wordpress_phpunit_child_script_seconds=6.031654`,
  `wordpress_phpunit_child_process_outer_minus_script_seconds=0.885012`,
  `wordpress_phpunit_child_process_runtime_ms_avg=988.095`,
  `wordpress_phpunit_child_script_ms_avg=861.665`, and
  `wordpress_phpunit_child_process_outer_minus_script_ms_avg=126.430` while
  full child profiling remained off.
- The WordPress timing summary contained the same child-script and
  outer-minus-script rows under `manual-child-script-ci-timing`.
- `ctest --preset prod -R
  'tools\.ci-production-builds|tools\.wordpress-phpunit-timing-rollup'
  --output-on-failure` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed before recording these verification notes.

## Acceptance Criteria

- The new flag defaults to `0`.
- The three process-isolated CI shards set the new flag to `1` together with
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`.
- The process-isolated CI shards keep
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`.
- The production audit fails if a process-isolated shard loses the new flag or
  enables full child profiling.
- The timing summary includes child-script and outer-minus-script rows for
  process-isolated CI shards.
- Database and non-isolated PHPUnit shards keep the global disabled default.

## Risks And Unresolved Questions

- Child-script time still includes PHPUnit child bootstrap, WordPress bootstrap,
  MyLite child open/close, SQL work, and test body work. It is narrower than
  child-process runtime, not a pure engine metric.
- The next optimization depends on the split: a small outer-minus-script value
  points at child script bootstrap/open-close/test work; a large value points at
  PHP process launch, temporary-file execution, pipe handling, or parent
  process overhead.
