# WordPress PHPUnit Child Process Averages

## Problem

The WordPress process-isolated PHPUnit step already prints child-process totals,
but CI readers still have to divide those totals by
`wordpress_phpunit_child_process_count` to see the per-process startup/runtime,
parent lock-release, and reconnect costs. That manual step is easy to miss when
investigating why the isolated suite is slow.

This slice adds per-child average timing keys to the existing PHPUnit vendor
patch without changing test execution.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `DefaultPhpProcess.php` during the `dependencies` phase so the parent closes
  MyLite-backed `wpdb` handles before each process-isolated child
  `proc_open()`, records lock-release time, records child-process runtime, and
  records reconnect time.
- The existing profile keys are totals:
  `wordpress_phpunit_child_process_lock_release_seconds`,
  `wordpress_phpunit_child_process_runtime_seconds`, and
  `wordpress_phpunit_child_process_reconnect_seconds`.
- Focused local production runs show the child-process runtime total dominates
  the process-isolated path; per-child values make that visible directly in CI
  logs.

## Design

Add a `MYLITE_WORDPRESS_PHPUNIT_CHILD_PROCESS_AVERAGES` marker to the injected
PHPUnit patch and print these derived keys at shutdown when profiling is
enabled:

- `wordpress_phpunit_child_process_lock_release_ms_avg`,
- `wordpress_phpunit_child_process_runtime_ms_avg`,
- `wordpress_phpunit_child_process_reconnect_ms_avg`.

The averages are derived from the already-collected totals and
`wordpress_phpunit_child_process_count`; no additional timing sections are
introduced.

Update the harness patcher so already-patched vendor trees that have the older
total-only timing block are upgraded in place. Fresh vendor trees receive the
new marker and keys in the initial injected block.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or WordPress
behavior changes. The slice changes only diagnostic lines emitted by the
WordPress PHPUnit harness.

## Directory And Lifecycle Impact

No durable directory-layout changes. The parent still closes and reconnects the
same detected `wpdb` handles around process-isolated children.

## Public API Impact

No public MyLite API changes.

## Native Storage Impact

No native storage format or recovery changes.

## Build And Performance Impact

The new output performs a few arithmetic operations in a shutdown function
after PHPUnit execution. It is outside the measured child process body and does
not change CI workload.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the production WordPress `dependencies` phase against the warmed
  PHPUnit vendor tree and verify the average marker is installed.
- Run a focused process-isolated WordPress class under the production harness
  and confirm the new average keys print with the existing total keys.
- Run `git diff --check`.
- Run `cmake --build --preset format-check-prod`.

## Verification Results

Local verification on 2026-06-08 used the production
`build/wordpress-php-embedded-prod` Release MyLite/PHP extension artifacts,
the `build/wordpress-mariadb-embedded` MinSizeRel MariaDB embedded archive, and
the default external WordPress test database path guarded by
`MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1`.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- Production `MYLITE_WORDPRESS_PHASE=dependencies` passed against the warmed
  WordPress/PHPUnit vendor tree and installed
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_PROCESS_AVERAGES` into
  `DefaultPhpProcess.php`.
- `rg` verified the patched vendor file contains the average marker and the
  three new `_ms_avg` output keys.
- Guarded production `prepare-db` passed and printed
  `wordpress_db_parent_filesystem_type=tmpfs` in the local container.
- Guarded focused `Tests_Formatting_Emoji` passed with 19 tests and printed
  the existing totals plus `wordpress_phpunit_child_process_lock_release_ms_avg=340.086`,
  `wordpress_phpunit_child_process_runtime_ms_avg=4686.045`, and
  `wordpress_phpunit_child_process_reconnect_ms_avg=111.340`.
- `git diff --check` passed.
- `cmake --build --preset format-check-prod` passed.

## Acceptance Criteria

- Existing child-process total keys remain unchanged.
- The three new per-child average keys print for process-isolated runs when
  profiling is enabled.
- Already-patched PHPUnit vendor trees upgrade without a clean dependency
  reinstall.
- No CI phase split or test filter changes are introduced.

## Risks And Unresolved Questions

- The averages expose per-child costs but do not reduce them.
- Average runtime still includes the child test body and WordPress bootstrap;
  it is not a pure `exec()` or MyLite open measurement.
