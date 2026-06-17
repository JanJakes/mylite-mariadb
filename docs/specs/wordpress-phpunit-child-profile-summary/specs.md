# WordPress PHPUnit Child Profile Summary

## Problem Statement

The WordPress PHPUnit harness can opt in to parent-side child-process
profiling with `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1`. Those
metrics show how much process-isolated PHPUnit time is spent releasing MyLite
database locks, running the child process, and reconnecting the parent. The
raw log already contains those keys, but the published timing summary omits
them, so CI or local profile runs still require log scraping to understand
per-child startup and reconnect cost.

This is a visibility gap, not a product runtime behavior gap. Normal CI keeps
child-process profiling disabled to avoid adding diagnostic overhead to timing
samples.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `DefaultPhpProcess.php` during the `dependencies` phase so the parent closes
  MyLite-backed `wpdb` handles before process-isolated children run.
- The existing injected patch emits `wordpress_phpunit_child_process_*` totals
  and per-child averages at parent PHP shutdown when
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1`.
- The same harness already captures PHPUnit output in a temporary file, parses
  `wordpress_phpunit_shell_real_seconds`,
  `wordpress_phpunit_reported_seconds`, and
  `wordpress_phpunit_shell_overhead_seconds`, and appends those values to
  `MYLITE_WORDPRESS_TIMING_SUMMARY_PATH`.
- The existing CI workflow keeps
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`, so normal production
  timing remains unaffected unless a diagnostic run explicitly opts in.

## Scope And Non-Goals

In scope:

- Append selected child-process profile keys to the WordPress timing summary
  when they are present in captured PHPUnit output.
- Preserve existing raw stdout/stderr metric names.
- Preserve the default unprofiled CI behavior.
- Record current focused production evidence for process-isolated child cost.

Out of scope:

- Enabling child-process profiling in normal CI timing steps.
- Enabling the heavier mysqli adapter profiler in normal CI.
- Changing PHPUnit filters, reconnect policy, static `wpdb` scanning, or
  keepalive behavior.
- Optimizing MyLite open/close, PHP startup, WordPress bootstrap, or engine SQL
  execution in this slice.

## Design

Extend the existing `phpunit` phase post-processing in
`tools/wordpress-phpunit-mysqli-mylite`:

- after parsing the normal PHPUnit shell/reported/overhead timings, scan the
  same captured output for child-process profile keys;
- append the last value of each key to the same `phpunit_summary_metrics`
  array, matching the existing mysqli-profile summary approach;
- append nothing when profiling is disabled or the run has no
  process-isolated children.

The selected keys are:

- `wordpress_phpunit_child_process_count`;
- `wordpress_phpunit_child_process_closed_wpdbs`;
- `wordpress_phpunit_child_process_static_properties`;
- `wordpress_phpunit_child_process_skipped_static_properties`;
- `wordpress_phpunit_child_process_lock_release_seconds`;
- `wordpress_phpunit_child_process_runtime_seconds`;
- `wordpress_phpunit_child_process_reconnect_seconds`;
- `wordpress_phpunit_child_process_lock_release_ms_avg`;
- `wordpress_phpunit_child_process_runtime_ms_avg`;
- `wordpress_phpunit_child_process_reconnect_ms_avg`.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or WordPress test
behavior changes. The change only copies existing opt-in diagnostic output into
the already-published timing summary.

## Directory And Lifecycle Impact

No durable directory layout or database lifecycle changes. The harness already
creates and removes the temporary PHPUnit output file; this slice reads a few
additional keys from that file before removal.

## Native Storage Impact

No native storage format, recovery, locking, or ownerless behavior changes.

## Build, Size, License, And Dependencies

No compiled-code, binary-size, license, or dependency changes. The added shell
loop runs only after a PHPUnit phase finishes and only appends keys that are
already present in the captured output.

## Performance Impact

Normal CI timing remains in the fast mode with
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`, so no child-profile
timing overhead is added to the default job. Opt-in profile runs add only
post-processing over a fixed list of ten keys after the suite finishes.

Current focused production profiling shows the process-isolated path is
dominated by PHPUnit/WordPress/PHP process work rather than parent reconnect
time. A single focused `Tests_Formatting_Emoji` method reported one child
process with `257.372ms` parent lock-release time, `4474.076ms` child runtime,
and `105.157ms` parent reconnect time, while the full one-test shell real time
was `28.805s`.

The refreshed ordinary WordPress perf probe on the same host reported PHP
wrapper startup `88.393ms`, process plus explicit MyLite connect/close
`595.468ms`, in-process mysqli connect/close `478.185ms`, active-runtime
reconnect `3.987ms`, `SELECT 1` `780.82 ops/s`, transactional inserts
`812.27 ops/s`, and point selects `713.76 ops/s`.

## Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a focused production WordPress process-isolated PHPUnit test with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` and a custom timing
  summary path.
- Verify the timing summary includes the child-process profile keys.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `git diff --check`.

## Verification Results

Completed on 2026-06-17 with the existing production
`build/wordpress-php-embedded-prod` Release build, the
`build/wordpress-mariadb-embedded` MinSizeRel archive, and the prepared
external `/tmp` WordPress MyLite database.

Syntax and CI guard checks passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite tools/check-ci-production-builds
tools/check-ci-production-builds
ci_production_build_audit_ok=/home/agent/.paseo/worktrees/1irk9sr4/greasy-ostrich/.github/workflows/ci.yml

ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
100% tests passed, 0 tests failed out of 1
```

A focused production WordPress process-isolated PHPUnit run with
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` passed one test and
appended the child-profile rows to the custom timing summary:

```text
MYLITE_WORDPRESS_PHASE=phpunit \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1 \
MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-child-summary \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/manual-wordpress-child-profile-summary-20260617/timing-summary.md \
tools/wordpress-phpunit-mysqli-mylite \
  --filter '^Tests_Formatting_Emoji::test_print_emoji_detection_script_on_front_end$'
```

The run reported `wordpress_phpunit_shell_real_seconds=18.202`,
`wordpress_phpunit_reported_seconds=6.037`,
`wordpress_phpunit_child_process_count=1`,
`wordpress_phpunit_child_process_closed_wpdbs=1`,
`wordpress_phpunit_child_process_lock_release_seconds=0.311974`,
`wordpress_phpunit_child_process_runtime_seconds=4.050282`,
`wordpress_phpunit_child_process_reconnect_seconds=0.134306`,
`wordpress_phpunit_child_process_lock_release_ms_avg=311.974`,
`wordpress_phpunit_child_process_runtime_ms_avg=4050.282`, and
`wordpress_phpunit_child_process_reconnect_ms_avg=134.306`. The timing summary
contained those same child-profile rows under the `manual-child-summary` label.

A matching focused run with
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` passed and left the custom
summary with only the existing `wordpress_phpunit_*`, container, and total
rows; it contained no `wordpress_phpunit_child_process_*` rows.

`git diff --check` also passed.

## Acceptance Criteria

- Unprofiled PHPUnit phases keep the existing timing summary shape.
- Opt-in child-profile PHPUnit phases append selected
  `wordpress_phpunit_child_process_*` rows to the same timing summary label as
  the normal PHPUnit phase timings.
- Raw child-process profile output remains unchanged.
- CI production-build and timing guard checks continue to pass.

## Risks And Follow-Up

- The new rows expose process-isolated startup/reconnect costs but do not
  reduce them.
- The child runtime metric includes PHP startup, WordPress bootstrap, and test
  body work inside each child process; it is not a pure MyLite engine metric.
- The next optimization work should target the remaining expensive startup and
  open/close path only if fresh branch/main probes show a reproducible
  regression outside normal runner noise.
