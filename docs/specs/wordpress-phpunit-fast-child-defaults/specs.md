# WordPress PHPUnit Fast Child Defaults

## Problem

The WordPress PHPUnit CI job already runs production builds and explicitly
disables diagnostic-only parent-side costs by default. At the time of this
slice, both child-process profiling and defensive static `wpdb` scanning were
disabled in CI and local/default harness runs still enabled both diagnostics.
That made a default focused process-isolated run look slower than the CI timing
mode and obscured the actual bottleneck, which is the child process's
WordPress/MyLite bootstrap. A later isolated-child timing slice kept the
reflection-heavy static scan disabled but opted the two process-isolated CI
steps back into the lightweight child count/runtime counters so CI can report
per-child attribution.

Current production evidence at `22fa5629`:

- `^Tests_DB` with CI-style fast settings passed 651 tests in
  `33.519s` shell real, with PHPUnit-reported test body time `16.301s`.
- The deferred process-isolated shard with static scan disabled and profiling
  enabled passed 50 tests in `198.115s` shell real. It launched 31 child
  processes with `46.021 ms` average parent lock-release time,
  `0.011 ms` reconnect time, and `5564.998 ms` average child runtime.
- A temporary CLI OPcache experiment on `Tests_Formatting_Emoji` still averaged
  `4859.217 ms` child runtime, which stayed inside the prior no-OPcache range
  and was rejected as a CI default change.

## Design

Change the harness defaults to match CI's fast process-isolated timing mode:

- `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES` defaults to `0`.
- `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN` defaults to `0`.

Both variables remain validated as `0` or `1` and remain forwarded into the
Docker container. Setting either variable to `1` keeps the existing diagnostic
behavior. The parent still closes the global WordPress `wpdb` before launching
PHPUnit children, so process-isolated children can open the ordinary MyLite
database directory.

The global default remains `0`; CI may override
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` for process-isolated
timing shards when the static scan remains disabled. That override records
child count, parent lock-release, child runtime, and reconnect timing without
enabling the slower reflection scan.

Do not enable CLI OPcache from this slice. The focused A/B did not show a
clear improvement, and changing PHP opcode-cache assumptions for the WordPress
suite needs stronger evidence.

## Compatibility Impact

No SQL, PHP API, mysqli API, storage, or WordPress test coverage changes. This
only changes default harness diagnostics. CI was already setting the fast mode
explicitly, so CI behavior is unchanged except that accidental omission of the
two environment variables no longer reintroduces slower diagnostic defaults.

## Build And Performance Impact

No production binary changes. Default local process-isolated PHPUnit runs avoid
diagnostic reflection/profiling work. The dominant remaining cost is still
child runtime, including PHP startup, WordPress bootstrap, and ordinary MyLite
open/close in the child process.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a focused production WordPress PHPUnit process-isolated class without
  overriding the two child-mode variables and confirm the harness logs `0` for
  both defaults.
- Run the production CI build guard.
- Run `git diff --check`.

## Acceptance Criteria

- Default harness logs show `wordpress_phpunit_profile_child_processes=0` and
  `wordpress_phpunit_static_wpdb_scan=0`.
- Explicit diagnostic opt-in remains available through the existing variables.
- Production build timing guards still pass.

## Verification Results

Local verification on 2026-06-10 used the production
`build/wordpress-php-embedded-prod` Release build, the
`build/wordpress-mariadb-embedded` MinSizeRel archive, the CI-pinned WordPress
ref `6ddfc9d9b532c6e95c1266165149815895e2eb56`, and the warmed WordPress
Docker image.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- A default-mode `MYLITE_WORDPRESS_PHASE=phpunit` run for
  `Tests_Formatting_Emoji` passed 19 tests with 69 assertions. The harness
  logged `wordpress_phpunit_profile_child_processes=0` and
  `wordpress_phpunit_static_wpdb_scan=0`, confirming the new defaults, and
  reported PHPUnit `Time: 00:23.219` with
  `wordpress_phpunit_shell_real_seconds=35.135`.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `tools/require-cmake-release-build build/prod build/php-embedded-prod
  build/wordpress-php-embedded-prod` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded
  build/wordpress-mariadb-embedded` passed.
- `git diff --check` passed.
