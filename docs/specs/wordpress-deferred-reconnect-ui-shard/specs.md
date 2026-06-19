# WordPress Deferred Reconnect UI Shard

## Problem

The WordPress PHPUnit job already splits build, setup, performance probe, and
test-only phases, and the process-isolated shards expose parent-side child
runtime, lock-release, and reconnect timing. One shard still forced parent
`wpdb` reconnect after every child even though its tests also used child
install skip. That kept avoidable parent reconnect work in the CI timing path.

## Source Findings

- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit
  `DefaultPhpProcess.php` so the parent closes MyLite-backed `wpdb` handles
  before `proc_open()` and can skip reconnect when
  `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0`.
- The existing process-isolated UI/filesystem/theme filter already sets
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`, so each child skips
  WordPress install work after the prepared database phase.
- The current child timing summary records parent reconnect time without
  enabling child-body profiling.

## Design

Rename the old eager-reconnect process-isolated filter to
`MYLITE_WORDPRESS_PHPUNIT_DEFERRED_RECONNECT_UI_FILTER` and run that shard with:

- `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0`;
- `MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1`;
- `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`;
- `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`.

Update the CI production-build audit so the renamed step keeps its production
build guards, child timing summary, child install skip, and reconnect-off
policy.

## Compatibility Impact

No SQL behavior, PHP API behavior, mysqli adapter behavior, public C API,
native storage behavior, or WordPress application behavior changes. This is a
CI/test-harness timing policy for a verified process-isolated subset.

## Directory And Lifecycle Impact

No durable directory-layout change. The parent still closes WordPress/MyLite
handles before each child. The difference is that the parent does not reopen a
new MyLite-backed `wpdb` handle between children for this proven shard.

## Build And Performance Impact

No compiled binary or dependency change. CI should spend less time in
parent-side MyLite reconnect work for this shard. The fresh local production
rerun through the renamed workflow variable reported parent reconnect average
`0.009 ms` per child with reconnect disabled.

## Verification Plan

- Refresh the WordPress production build cache when stale.
- Run the renamed shard's filter with reconnect disabled, child install skip,
  and child timing summary.
- Run `bash -n` for the WordPress harness and CI audit script.
- Run `tools/check-ci-production-builds`.
- Run the production CTest wrapper for the CI audit.
- Run formatting and whitespace checks.

## Verification Results

Local verification on 2026-06-19 used `Release`
`build/wordpress-php-embedded-prod`, `MinSizeRel`
`build/wordpress-mariadb-embedded`, the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, and the external `/tmp`
WordPress MyLite database path.

The production WordPress build cache was refreshed first because the guard
correctly rejected a stale MariaDB embedded archive older than
`mariadb/storage/innobase/mtr/mtr0mtr.cc`:

```text
MYLITE_WORDPRESS_PHASE=build-php
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release
MYLITE_WORDPRESS_MARIADB_BUILD_DIR=build/wordpress-mariadb-embedded
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1
tools/wordpress-phpunit-mysqli-mylite
```

The refreshed build reported `mylite_mariadb_embedded_seconds=15`,
`mylite_php_build_seconds=22`, and `mylite_build_seconds=39`.

The former eager filter first passed with reconnect disabled:

```text
MYLITE_WORDPRESS_PHASE=phpunit
MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0
MYLITE_WORDPRESS_PHPUNIT_CHILD_TIMING_SUMMARY=1
MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1
MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1
tools/wordpress-phpunit-mysqli-mylite --filter "$filter"
```

Results:

- `22` tests, `67` assertions passed;
- `wordpress_phpunit_reported_seconds=23.634`;
- `wordpress_phpunit_shell_real_seconds=32.982`;
- `wordpress_phpunit_child_process_count=22`;
- `wordpress_phpunit_child_process_closed_wpdbs=5`;
- `wordpress_phpunit_child_process_lock_release_ms_avg=11.195`;
- `wordpress_phpunit_child_process_runtime_ms_avg=995.402`;
- `wordpress_phpunit_child_process_reconnect_ms_avg=0.015`.

After the workflow variable and step were renamed, the same filter was rerun
through `MYLITE_WORDPRESS_PHPUNIT_DEFERRED_RECONNECT_UI_FILTER` and passed
again:

- `22` tests, `67` assertions passed;
- `wordpress_phpunit_reported_seconds=28.749`;
- `wordpress_phpunit_shell_real_seconds=38.197`;
- `wordpress_phpunit_child_process_count=22`;
- `wordpress_phpunit_child_process_closed_wpdbs=5`;
- `wordpress_phpunit_child_process_lock_release_ms_avg=13.020`;
- `wordpress_phpunit_child_process_runtime_ms_avg=1221.746`;
- `wordpress_phpunit_child_process_reconnect_ms_avg=0.009`.

Additional checks passed:

- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `bash -n tools/check-ci-production-builds`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Acceptance Criteria

- CI no longer names or runs this shard as eager reconnect.
- The renamed shard keeps child install skip and child timing summary enabled.
- The renamed shard explicitly disables parent reconnect after child.
- The CI production audit fails if the shard loses production guards or
  reconnects after each child again.

## Risks And Follow-Up

- This only removes parent reconnect work from a verified process-isolated
  subset. Each child still pays its own PHP, WordPress bootstrap, and MyLite
  lifecycle cost.
- If the filter changes, the new test set must be rerun with reconnect disabled
  before keeping this policy.
