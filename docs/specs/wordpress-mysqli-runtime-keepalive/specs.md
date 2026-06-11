# WordPress MySQLi Runtime Keepalive

## Problem

The production WordPress PHPUnit split made the slow phase visible: the
non-isolated remaining suite is dominated by the test body, not setup or PHP
extension build work. The first full production profile run on this branch
reported `wordpress_phpunit_reported_seconds=1481.301` for that step.

The mysqli adapter profile from the same run showed `1015` MyLite opens and
closes in the main non-isolated PHPUnit process. Those lifecycle calls took
`open_ms_total=78267.045` and `close_ms_total=234811.889`, about `313` seconds
of a `1481` second PHPUnit body. The performance probe from the same CI run
measured full process connect/close at `323.238 ms` and active-runtime
reconnect at `2.059 ms`, so the repeated full embedded runtime shutdown path is
a concrete performance target.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/api/php-extensions.md` defines `mysqli_mylite` as an optional
  mysqli-shaped adapter over `libmylite`; the mysqli host argument is a MyLite
  database directory path, not a daemon address.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` maps
  `mysqli_real_connect()` and `mysqli_connect()` to
  `php_mylite_mysqli_open_link()`, which calls `mylite_open()`.
- The same adapter maps explicit `mysqli_close()` and PHP object destruction to
  `php_mylite_mysqli_profiled_close()`, which calls `mylite_close()`.
- WordPress `wpdb::db_connect()` creates a new mysqli object through
  `mysqli_init()` and `mysqli_real_connect()`, then selects the configured test
  database. `wpdb::close()` calls `mysqli_close()` and clears `$wpdb->dbh`,
  `$wpdb->ready`, and `$wpdb->has_connected`.
- WordPress includes explicit close/reconnect tests in `tests/phpunit/tests/db.php`.
  Those tests must continue to see individual mysqli handles close normally.
- WordPress `tests/phpunit/includes/bootstrap.php` runs
  `tests/phpunit/includes/install.php` through `system( WP_PHP_BINARY ... )`
  before the test suite starts unless `WP_TESTS_SKIP_INSTALL=1`. A parent
  keepalive cannot be open during that install subprocess when using ordinary
  non-ownerless MyLite opens, so the keepalive must start after the normal
  WordPress bootstrap.
- The WordPress harness already keeps process-isolated tests in separate CI
  shards and patches PHPUnit child-process handling to close parent `wpdb`
  handles around child execution. A parent-process keepalive should not be used
  for those shards because it would intentionally keep a database connection
  open while a child process is trying to run independently.

## Design

Add an opt-in WordPress PHPUnit harness flag,
`MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`.

When enabled for a PHPUnit phase, the harness creates a temporary PHPUnit
bootstrap wrapper. The wrapper first requires WordPress'
`tests/phpunit/includes/bootstrap.php` so the normal install child process and
initial WordPress database bootstrap run without a parent keepalive lock. It
then opens one MyLite-backed global `mysqli` connection to
`MYLITE_WORDPRESS_DB_DIR`, stores it in
`$GLOBALS['myliteWordPressMysqliKeepalive']`, and registers a shutdown function
that explicitly closes it. This keeps the embedded runtime active during test
execution while short-lived WordPress mysqli objects open and close in the
same PHP process. Individual application-visible mysqli handles still close
through the normal adapter path, so `wpdb::close()` behavior and explicit close
tests are not bypassed.

The existing PHPUnit child-process patch also closes the keepalive before
`proc_open()` and reopens it after the child exits. That preserves the
ordinary single-process MyLite directory lock for subprocesses while retaining
the runtime between non-isolated in-process test connections.

The flag defaults to `0`. CI enables it only for the slow non-isolated
remaining WordPress PHPUnit step, together with
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, so production logs show whether
open/close time collapses toward the active-runtime reconnect cost. The
database suite and both process-isolated shards keep the flag off.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, or application behavior changes.
This is a WordPress harness behavior, not a change to `mylite_close()` or
`mysqli_mylite` default semantics.

The keepalive intentionally models the server side of a normal MySQL/MariaDB
deployment for the non-isolated application suite: individual client
connections can come and go while the server runtime stays alive. It is not
used as lifecycle evidence for final-close cleanup or process-isolated parent
and child ownership.

## Directory And Lifecycle Impact

No durable files are created outside the MyLite database directory. The
temporary bootstrap wrapper lives under the process temporary directory and is
removed by the harness after PHPUnit exits.

While the keepalive is enabled, final embedded shutdown for the WordPress test
database is deferred until the PHPUnit process exits and the keepalive shutdown
function calls `mysqli_close()`. This is intentionally limited to the
non-isolated WordPress harness step.

## Build, Size, And Dependency Impact

No compiled code, binary size, or dependency changes. The change is shell/PHP
harness code plus CI/audit wiring.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest audit for `tools.ci-production-builds`.
- Run a focused production WordPress PHPUnit `^Tests_DB` sample with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1` and
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, verifying the close/reconnect
  tests still pass and the keepalive open/close markers appear.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- The keepalive is disabled unless `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`.
- Enabling the keepalive opens exactly one harness-owned mysqli connection
  after WordPress bootstrap and closes it at process shutdown.
- Explicit WordPress database close/reconnect tests continue to pass with the
  keepalive enabled.
- CI keeps production `Release`/`MinSizeRel` guards on every timing-producing
  WordPress step and enables the keepalive only for the non-isolated remaining
  PHPUnit shard.
- The non-isolated shard keeps mysqli profiling enabled so the next CI run
  reports whether repeated full open/close cost was reduced.

## Verification Results

Local verification on 2026-06-11 used production build caches:
`build/wordpress-php-embedded-prod` with `Release` MyLite,
`build/wordpress-mariadb-embedded` with `MinSizeRel` MariaDB embedded, and a
`/tmp`-mounted WordPress MyLite test database outside the repository worktree.
The WordPress checkout was the CI-pinned ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `MYLITE_WORDPRESS_PHASE=dependencies ... tools/wordpress-phpunit-mysqli-mylite`
  passed and upgraded the cached PHPUnit `DefaultPhpProcess.php`; the generated
  vendor file contains `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE_LOCK_RELEASE` and
  `myliteWordPressReopenKeepaliveForChild()`.
- Focused production WordPress PHPUnit with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, and `--filter '^Tests_DB'`
  passed `651` tests with `3` skips. The run reported
  `wordpress_phpunit_keepalive_opened=1`,
  `wordpress_phpunit_keepalive_closed=1`,
  `wordpress_phpunit_reported_seconds=18.619`,
  `wordpress_phpunit_shell_real_seconds=36.067`, `open_calls=11`,
  `open_ms_total=379.113`, `close_calls=11`, and
  `close_ms_total=551.476`.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Open Questions

- The keepalive reduces test-harness cold lifecycle churn. It does not optimize
  the default `mylite_open()`/`mylite_close()` path or change the documented
  final-close cleanup contract.
- The non-isolated suite still spends substantial time in query execution and
  result materialization. If the next profile shows lifecycle time is no longer
  the long pole, the next bounded slice should split result-query and status
  synchronization subphases more finely or optimize the hot query path.
- Do not enable the keepalive for process-isolated shards without a separate
  source review of parent/child database ownership and lock release behavior.
