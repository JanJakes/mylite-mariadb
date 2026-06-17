# WordPress PHPUnit Isolated Suite Split

## Problem

The WordPress PHPUnit job now separates build/setup work from test execution,
but the `^(?!Tests_DB)` remaining-suite step still mixes ordinary tests with
PHPUnit tests that run in separate PHP child processes. Focused profiling shows
those child-process tests have a different cost model from ordinary PHPUnit:
the parent must close MyLite-backed `wpdb` handles, the child starts PHP and
opens the MyLite database directory, then the parent reconnects.

Keeping process-isolated tests inside the same remaining-suite step makes CI
timings hard to interpret and can make ordinary PHPUnit throughput look worse
than it is.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` already emits
  `wordpress_phpunit_child_process_*` totals and per-child averages when
  process-isolated tests run.
- Current production WordPress perf probe on 2026-06-08 reported:
  stock PHP process startup `60.313 ms`, MyLite PHP process startup
  `82.894 ms`, process-plus-connect `627.722 ms`, connect delta
  `544.828 ms`, and in-process connect/close `446.075 ms`.
- Focused `Tests_Formatting_Emoji` run under `php-embedded-prod` passed
  `19` tests in `34` wrapper seconds and emitted `4` child processes,
  `1.102582 s` lock release, `17.687319 s` child runtime, and
  `0.441773 s` reconnect.
- Focused class-level `Tests_Admin_ExportWp` run passed `11` tests in `88`
  wrapper seconds and emitted `11` child processes, `3.046033 s` lock release,
  `68.578563 s` child runtime, and `1.376416 s` reconnect.
- The exact process-isolated CI filter against the pinned WordPress ref
  `6ddfc9d9b532c6e95c1266165149815895e2eb56` passed `321` tests in `401`
  wrapper seconds. PHPUnit reported `5` upstream deprecation warnings and `1`
  skipped test, and the MyLite child-process profile reported `53` child
  processes, `16.321770 s` lock release, `276.757446 s` child runtime, and
  `6.728554 s` reconnect.
- The pinned WordPress checkout contains process-isolation annotations in
  these classes:
  `Tests_Admin_ExportWp`, `Tests_Admin_WpAutomaticUpdater`,
  `Tests_Admin_WpUpgrader`, `Tests_Ajax_wpAjaxResponse`,
  `Tests_Filesystem_WpFilesystemDirect_Chmod`,
  `Tests_Filesystem_WpFilesystemDirect_Mkdir`, `Tests_Formatting_Emoji`,
  `Tests_Functions_WpUniquePrefixedId`, `Tests_oEmbed_HTTP_Headers`,
  `Tests_Sitemaps_Sitemaps`, and `Tests_Theme`.

## Design

Add two CI-level filter variables for the pinned WordPress checkout:

- `MYLITE_WORDPRESS_PHPUNIT_ISOLATED_FILTER`, matching the process-isolated
  classes listed above;
- `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER`, excluding `Tests_DB` and the
  same process-isolated classes.

Keep the existing `Tests_DB` step unchanged. Replace the single remaining-suite
step with:

- `Run WordPress PHPUnit process-isolated suite (test only)`;
- `Run WordPress PHPUnit non-isolated remaining suite (test only)`.

This is a timing split only. It does not remove tests from CI; it makes the
process-isolated child-process path visible in its own job step with the
existing child-process timing keys. A later CI timing slice enables the
lightweight child-process profile on the deferred and eager process-isolated
steps so those keys are appended to the timing summary by default for CI,
while leaving the slower static `wpdb` scan disabled.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or directory-layout
change. WordPress PHPUnit coverage remains the same modulo class-level grouping
of tests that already request process isolation.

## Performance Impact

No runtime optimization. CI logs now distinguish ordinary remaining-suite time
from process-isolated child-process time. The split also makes future tuning of
MyLite open/connect and WordPress child-process policy measurable without
masking it inside the ordinary suite.

## Test Plan

- Run focused process-isolated classes under the production WordPress harness:
  `Tests_Formatting_Emoji` and `Tests_Admin_ExportWp`.
- Run the exact process-isolated CI filter under the production WordPress
  harness.
- Validate the isolated and non-isolated filters with PHP PCRE against
  representative included and excluded PHPUnit test names.
- Run the WordPress mysqli `perf-probe` phase with production PHP build
  artifacts.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `git diff --check`.

## Acceptance Criteria

- CI has separate WordPress PHPUnit steps for database, process-isolated, and
  non-isolated remaining tests.
- The process-isolated steps use the existing child-process profile output.
- The non-isolated remaining step excludes both `Tests_DB` and the
  process-isolated classes.
- The split uses the existing production PHP build directory and Release build
  type.
