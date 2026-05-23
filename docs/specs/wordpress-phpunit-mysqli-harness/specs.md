# WordPress PHPUnit mysqli Harness

## Problem

WordPress is a practical compatibility target for the PHP `mysqli` adapter. The
first harness should fetch `wordpress-develop`, load MyLite's PHP core and
`mysqli_mylite` modules in a Linux PHP runtime, run WordPress' PHPUnit suite
against a MyLite database directory, and report elapsed time. The full suite is
also a CI regression once the local runtime and compatibility result are known.

## Source Findings

- WordPress `trunk` uses `src/wp-includes/class-wpdb.php` as its mysqli
  integration point.
- The bootstrap path calls `mysqli_report()`, `mysqli_init()`,
  `mysqli_real_connect()`, `mysqli_set_charset()`, `mysqli_query()`,
  `mysqli_fetch_array()`, `mysqli_select_db()`, result cleanup helpers, error
  helpers, insert/affected-row helpers, column metadata helpers, and server
  version helpers.
- The test installer in `tests/phpunit/includes/install.php` creates and drops
  the normal WordPress tables after selecting `DB_NAME`.

## Design

- Add a local runner at `tools/wordpress-phpunit-mysqli-mylite`.
- Wire the runner into CI as `wordpress-phpunit-mysqli-mylite`.
- Build a Docker image from `php:8.3-cli-bookworm` with the MariaDB/MyLite build
  dependencies, Composer, `gd`, and `zip`.
- Inside Docker, build the MariaDB embedded archive and MyLite PHP extensions
  for the container PHP ABI.
- Fetch `wordpress-develop` into `build/wordpress-develop`. The local default
  remains `trunk`; CI pins the WordPress ref to
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`, the commit proven by the first
  full-suite pass, so CI failures are attributable to MyLite or the pinned
  compatibility target rather than unrelated upstream movement.
- Install WordPress Composer dependencies and a local PHPUnit 9.6 tool install.
- Generate `wp-tests-config.php` pointing `DB_HOST` to
  `localhost:/work/build/wordpress-tests.mylite` and `DB_NAME` to
  `wordpress_tests`. WordPress parses absolute paths in `DB_HOST` as socket
  paths, so the harness passes the MyLite database directory through mysqli's
  socket parameter.
- Run PHPUnit through a wrapper that always loads `mylite.so` and
  `mysqli_mylite.so`.
- Print phase timings, including `wordpress_phpunit_seconds`.

## Compatibility Impact

The `mysqli_mylite` adapter now covers the mysqli symbols WordPress reaches
during bootstrap. The adapter still has partial mysqli coverage; this harness is
expected to expose the next SQL, metadata, transaction, or API gaps.

For WordPress' parsed socket form, `mysqli_real_connect()` and
`mysqli_connect()` treat a non-empty socket argument as the MyLite database
directory path. Direct MyLite PHP calls can still pass the directory as the
first host argument.

## Directory Lifecycle

The harness removes and recreates `build/wordpress-tests.mylite` on each run.
All durable WordPress database state for the run stays inside that MyLite-owned
directory.

## Non-Goals

- No attempt to track moving WordPress trunk in required CI before MyLite has a
  broader application-compatibility triage process.
- No WordPress source vendoring.
- No attempt to make all WordPress tests pass before measuring the first local
  run.
- No support for stock `ext/mysqli` coexistence in the WordPress run; the Docker
  PHP image relies on `mysqli_mylite` registering global `mysqli_*` symbols.

## Verification

Run:

```sh
tools/wordpress-phpunit-mysqli-mylite
```

CI runs the same command through the `wordpress-phpunit-mysqli-mylite` GitHub
Actions job, with `MYLITE_WORDPRESS_REF` pinned to the known-good WordPress
commit.

For shorter local probes, pass normal PHPUnit arguments after the script name,
for example:

```sh
tools/wordpress-phpunit-mysqli-mylite --filter Tests_DB
```

Also run the normal MyLite PHP extension tests after changing mysqli behavior:

```sh
ctest --preset php-embedded-dev -R 'php-ext-mysqli-mylite.api' --output-on-failure
```

## Acceptance Criteria

- The runner fetches WordPress develop without committing it.
- The runner builds the Linux PHP modules inside Docker.
- WordPress starts under PHPUnit using MyLite's global mysqli replacement.
- The runner reports elapsed time for the PHPUnit phase.
- Any failure is a compatibility result from the run, not a missing harness
  dependency or missing mysqli bootstrap symbol.
- GitHub Actions runs the full WordPress PHPUnit suite as a regular CI signal
  on the pinned compatibility target.
