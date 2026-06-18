# WordPress PHPUnit Child Timing Summary

## Problem

The WordPress PHPUnit CI job now splits setup, build, perf-probe, and test-only
steps, and it keeps the heavyweight process-isolated child profiler disabled in
normal timing steps. That keeps CI closer to production behavior, but it also
hides the per-child process cost that explains much of the branch's PHPUnit
wall time.

The existing diagnostic profiler can report child count, parent lock-release
time, child runtime, reconnect time, child-script runtime, and child mysqli
profile aggregates, but enabling it injects child-body timing code and extra
profile output. CI needs the low-overhead parent-side subset by default.

## Source Findings

- MyLite targets MariaDB 11.8 LTS from base tag `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not change
  MariaDB source, SQL semantics, native storage behavior, or compiled MyLite
  code.
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `DefaultPhpProcess.php` during the dependency phase so process-isolated
  children can run after parent MyLite-backed `wpdb` handles are closed.
- The patch already has parent-side timing buckets for lock release, child
  process runtime, and parent reconnect, but those buckets are currently tied
  to `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES`.
- `.github/workflows/ci.yml` deliberately sets
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` for the normal
  production timing job, and `tools/check-ci-production-builds` forbids the
  process-isolated timing steps from overriding it back to `1`.
- The WordPress timing summary already collects child-process keys when they
  appear in PHPUnit output and publishes the compact table to the GitHub step
  summary.

## Design

Add a separate opt-in harness flag:

`MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`

When enabled, the patched PHPUnit parent records the existing parent-side child
timing buckets without enabling child-body script instrumentation or mysqli
profiling. The emitted keys are the existing summary rows:

- `wordpress_phpunit_child_process_count`
- `wordpress_phpunit_child_process_closed_wpdbs`
- `wordpress_phpunit_child_process_lock_release_seconds`
- `wordpress_phpunit_child_process_runtime_seconds`
- `wordpress_phpunit_child_process_reconnect_seconds`
- `wordpress_phpunit_child_process_lock_release_ms_avg`
- `wordpress_phpunit_child_process_runtime_ms_avg`
- `wordpress_phpunit_child_process_reconnect_ms_avg`

The child-script temp-file path stays guarded by
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES != 0`, so the new summary
mode does not inject child-script timing work into normal CI children.

CI keeps `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=0` as the job default
and enables it only in the three process-isolated PHPUnit steps. The production
build audit enforces both halves: child profiling remains off, while the
lightweight child timing summary remains on for those shards.

## Affected Subsystems

- WordPress PHPUnit harness script.
- GitHub Actions WordPress PHPUnit timing job.
- CI production-build/timing audit.
- Compatibility and ownerless performance documentation.

## Compatibility Impact

No MySQL, MariaDB, SQL, mysqli, C API, native storage, or WordPress application
behavior changes. The process-isolated child execution, parent lock release,
and reconnect policies remain unchanged. Only diagnostic rows are added.

## Database Directory And Embedded Lifecycle Impact

No durable directory-layout change. Parent MyLite-backed `wpdb` handles are
still closed before process-isolated children. The new timing summary observes
that lifecycle path but does not keep additional MyLite connections open.

## Native Storage Impact

None. The slice does not alter InnoDB, MyISAM, Aria, redo, checkpoint, or
ownerless page-version code.

## Build, Size, License, And Dependency Impact

No compiled binary, dependency, license, or build-profile impact. The added
work is PHP parent-side `microtime(true)` accounting and shell environment
wiring in the existing harness.

## Test And Verification Plan

- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `bash -n tools/check-ci-production-builds`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- A focused production WordPress process-isolated PHPUnit run with
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1` and
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`, verifying the parent
  child timing keys appear.
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Verification Results

Local verification on 2026-06-18 used the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, production
`build/wordpress-php-embedded-prod` Release MyLite/PHP artifacts, the
`build/wordpress-mariadb-embedded` MinSizeRel MariaDB embedded archive, and
the default external tmpfs WordPress MyLite database path.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `bash -n tools/check-ci-production-builds` passed.
- `tools/check-ci-production-builds` passed.
- The guarded WordPress `dependencies` phase passed against the cached PHPUnit
  vendor tree and installed
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_PROFILE_GUARD`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_OUTPUT_PROFILE_GUARD`, and
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_AVERAGE_PROFILE_GUARD` into
  `DefaultPhpProcess.php`. A repeat dependency phase also passed, proving the
  patch is idempotent on the upgraded vendor file.
- Focused production process-isolated PHPUnit smoke passed with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`,
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`,
  `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`, and
  `--filter 'Tests_Functions_WpUniquePrefixedId::test_should_create_unique_prefixed_ids'`.
  It ran `7` tests with `14` assertions and reported
  `wordpress_phpunit_child_process_count=7`,
  `wordpress_phpunit_child_process_runtime_ms_avg=1506.584`,
  `wordpress_phpunit_child_process_lock_release_ms_avg=257.155`, and
  `wordpress_phpunit_child_process_reconnect_ms_avg=114.787` without enabling
  child-body script timing rows.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- The new flag defaults to `0`.
- Normal CI process-isolated timing keeps
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`.
- The three process-isolated CI shards set
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`.
- Child timing summary rows appear in the WordPress timing summary when the
  flag is enabled.
- Child-script timing remains tied to the heavier diagnostic child profiler,
  not the lightweight summary flag.

## Risks And Unresolved Questions

- Parent-side child runtime includes PHP startup, WordPress bootstrap, MyLite
  child open/close, SQL work, and test body work. It is not a pure engine
  microbenchmark.
- The timing adds a few parent-side `microtime(true)` calls per child. That is
  intentionally much lighter than child-body script timing and mysqli profile
  aggregation, but it is still diagnostic code in CI.
- This slice exposes per-child cost in CI. The larger optimization target
  remains reducing MariaDB embedded startup/shutdown cost or reducing the
  number of process-isolated MyLite child opens.
