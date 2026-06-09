# WordPress PHPUnit Deferred Reconnect Shards

## Problem

The WordPress process-isolated PHPUnit step is now separated from builds and
uses production MyLite artifacts, but it remains slow because every
process-isolated child needs the parent process to release the ordinary MyLite
database directory lock. The harness currently closes the global WordPress
`wpdb` before each child and eagerly reconnects it after each child. A focused
production sample on this branch still shows the ordinary mysqli
process-plus-connect path around `623.776ms`, in-process connect/close around
`380.918ms`, and active-runtime reconnect around `3.343ms`.

The parent reconnect is required for mixed classes that execute parent-side
database or escaping code between process-isolated children. A previous blanket
deferred-reconnect prototype failed `Tests_Formatting_Emoji` with parent-side
errors, so CI cannot simply keep `wpdb` disconnected for the entire isolated
suite.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- WordPress ref used by CI: `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `vendor/phpunit/phpunit/src/Util/PHP/DefaultPhpProcess.php` during the
  `dependencies` phase so the parent closes MyLite-backed `wpdb` handles
  before `proc_open()` and reconnects them after `proc_close()`.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens WordPress
  mysqli connections with `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE`, so the
  parent and child cannot both hold the ordinary directory lock.
- `docs/specs/wordpress-phpunit-static-scan-mode/specs.md` records that
  disabling the static `wpdb` scan in CI reduced focused `Tests_Formatting_Emoji`
  wall time, while a deferred-reconnect prototype failed later parent-side
  tests in that class.
- The current CI isolated filter is a pinned class set:
  `Tests_Admin_ExportWp`, `Tests_Admin_WpAutomaticUpdater`,
  `Tests_Admin_WpUpgrader`, `Tests_Ajax_wpAjaxResponse`,
  `Tests_Filesystem_WpFilesystemDirect_Chmod`,
  `Tests_Filesystem_WpFilesystemDirect_Mkdir`, `Tests_Formatting_Emoji`,
  `Tests_Functions_WpUniquePrefixedId`, `Tests_oEmbed_HTTP_Headers`,
  `Tests_Sitemaps_Sitemaps`, and `Tests_Theme`.

## Design

Add a guarded harness mode:

- `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=1` remains the default.
- `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0` keeps already-closed
  `wpdb` handles disconnected after a process-isolated child exits.
- The parent still closes `$GLOBALS['wpdb']` before `proc_open()` in both
  modes, so the child can take the ordinary MyLite directory lock.
- The patcher adds a stable
  `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD` marker and upgrades warmed
  patched PHPUnit vendor trees in place.

Use the deferred mode only for CI filter shards that pass focused production
testing. Keep eager reconnect for any class that needs parent-side DB access
between children, including the known-failing `Tests_Formatting_Emoji` class.

The proven deferred reconnect CI shard is:

- `Tests_Admin_ExportWp`
- `Tests_Filesystem_WpFilesystemDirect_Chmod`
- `Tests_Functions_WpUniquePrefixedId`
- `Tests_oEmbed_HTTP_Headers`
- `Tests_Sitemaps_Sitemaps`

The eager reconnect CI shard remains:

- `Tests_Admin_WpAutomaticUpdater`
- `Tests_Admin_WpUpgrader`
- `Tests_Ajax_wpAjaxResponse`
- `Tests_Filesystem_WpFilesystemDirect_Mkdir`
- `Tests_Formatting_Emoji`
- `Tests_Theme`

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, or native storage behavior changes.
The default harness behavior remains eager reconnect. Deferred reconnect is a
WordPress PHPUnit harness mode used only for proven test shards.

## Directory And Lifecycle Impact

No durable directory-layout change. Deferred shards reduce repeated parent
ordinary-runtime reopen churn by leaving the parent `wpdb` closed between
process-isolated children, while each child still opens and closes the same
external WordPress MyLite database directory.

## Build And Performance Impact

The change can reduce process-isolated CI wall time for safe shards by skipping
parent reconnect work and avoiding repeated parent close work after the first
child leaves `wpdb` disconnected. It does not reduce the child process's own
WordPress bootstrap or MyLite open/close cost.

Focused production evidence:

- A broad deferred reconnect trial over every process-isolated class except
  `Tests_Formatting_Emoji` failed with 170 parent-side `wpdb` connection errors
  in about `288.912s`, proving the optimization cannot be blanket-applied.
- A smaller deferred trial including
  `Tests_Filesystem_WpFilesystemDirect_Chmod`,
  `Tests_Filesystem_WpFilesystemDirect_Mkdir`,
  `Tests_Functions_WpUniquePrefixedId`, and
  `Tests_oEmbed_HTTP_Headers` failed only
  `Tests_Filesystem_WpFilesystemDirect_Mkdir::test_should_set_chmod`, so
  `Mkdir` stays eager.
- The final deferred trial for `Tests_Filesystem_WpFilesystemDirect_Chmod`,
  `Tests_Functions_WpUniquePrefixedId`, and
  `Tests_oEmbed_HTTP_Headers` passed 20 tests in `88.635s` with the known
  PHPUnit deprecation warnings.
- A second candidate trial over `Tests_Admin_ExportWp`,
  `Tests_Admin_WpUpgrader`, and `Tests_Sitemaps_Sitemaps` failed 64
  `Tests_Admin_WpUpgrader` parent-side DB errors, so `WpUpgrader` stays eager.
- The final deferred trial for `Tests_Admin_ExportWp` and
  `Tests_Sitemaps_Sitemaps` passed 30 tests in `114.782s`.
- The combined CI-shaped deferred shard passed 50 tests in `199.583s` with the
  same known PHPUnit deprecation warnings.
- The complementary CI-shaped eager shard passed 271 tests in `199.256s`,
  preserving eager reconnect for DB-dependent process-isolated classes.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Verify invalid `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD` values are
  rejected before Docker starts.
- Run guarded production `MYLITE_WORDPRESS_PHASE=dependencies` on a warmed
  PHPUnit vendor tree and verify the reconnect marker is installed.
- Run focused production process-isolated class filters with reconnect disabled
  to identify safe shards.
- Keep reconnect enabled for failing or unproven classes.
- Run the final CI-shaped process-isolated shard commands.
- Run production build-type guards, `cmake --build --preset format-check-prod`,
  and `git diff --check`.

## Acceptance Criteria

- Default WordPress PHPUnit behavior remains eager reconnect.
- Deferred reconnect mode is opt-in, validated, and visible in harness logs.
- CI uses deferred reconnect only for class filters proven by production
  focused runs.
- The eager reconnect shard still covers `Tests_Formatting_Emoji`.
- PHPUnit test-only steps remain separate from production builds.

## Risks And Follow-Up

- WordPress class behavior can change with the pinned WordPress ref. If the ref
  changes, the safe deferred shard list must be revalidated.
- This slice does not remove the full MyLite startup/shutdown cost for each
  child process. It only removes avoidable parent reconnect churn where the
  parent does not need the database between children.
