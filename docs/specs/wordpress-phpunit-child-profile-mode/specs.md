# WordPress PHPUnit Child Profile Mode

## Problem

The WordPress PHPUnit CI job now reports test-only timings from production
builds. The harness also injects parent-side child-process profiling around
process-isolated PHPUnit tests, but that profiling costs time in the measured
suite after the expensive static `wpdb` scan has already been disabled.

CI needs the lowest-noise PHPUnit wall time. Detailed per-child diagnostics can
remain available locally because the separate WordPress mysqli performance
probe already reports production process startup, connect/close, in-process
connect/close, active-runtime reconnect, read, and write timings.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- WordPress ref used by CI: `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `DefaultPhpProcess.php` so the parent closes WordPress `wpdb` handles before
  `proc_open()` and reconnects them after the child exits.
- `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` skips only the
  parent-side microtime accounting and shutdown summary output. The close,
  child launch, and reconnect behavior remains unchanged.
- Same-session production A/B on `Tests_Formatting_Emoji` with
  `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0` measured `28.711s` shell real
  and `29.842s` outer wall with child profiling disabled, versus `30.354s`
  shell real and `31.896s` outer wall with child profiling enabled.

## Design

The original child-profile mode kept local harness defaults
diagnostic-friendly:

- `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES` defaulted to `1`.
- The harness now validates the variable as `0` or `1` before Docker starts.
- The WordPress CI job sets
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` so CI test-only timings
  do not include child-profiling overhead.

A later fast-defaults slice changed the harness default to `0` as well, so
local/default timing now matches CI unless diagnostics are explicitly requested
with `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1`.

The static `wpdb` scan mode remains independent. CI disables both static scan
and child profiling, while local runs can enable either diagnostic mode.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or ownerless
coordination behavior changes. This is CI harness timing configuration only.

## Build And Performance Impact

No build output changes. The CI WordPress PHPUnit timing job keeps Release
MyLite PHP-extension artifacts and a `MinSizeRel` MariaDB embedded archive, but
stops paying for per-child profiling counters during process-isolated tests.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Verify invalid `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES` values are
  rejected before Docker starts.
- Run focused production `Tests_Formatting_Emoji` with child profiling
  disabled and static scan disabled.
- Run the same focused test with child profiling enabled and static scan
  disabled for an A/B timing sample.
- Run `git diff --check`.

## Acceptance Criteria

- CI explicitly disables child-process profiling for WordPress PHPUnit timing
  while retaining production build guards.
- Child-process profiling remains available for diagnostics.
- Invalid child-profile mode values fail early.
- Focused process-isolated PHPUnit coverage still passes with profiling
  disabled.

## Verification Results

Local verification on 2026-06-09 used WordPress checkout
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, the guarded
`build/wordpress-php-embedded-prod` Release CMake build, the guarded
`build/wordpress-mariadb-embedded` MinSizeRel MariaDB embedded archive, and a
WordPress MyLite test database mounted from host `/tmp`.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=bad
  tools/wordpress-phpunit-mysqli-mylite` rejected the invalid value before
  Docker startup.
- Focused production `Tests_Formatting_Emoji` passed with
  `MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0` and
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0`. PHPUnit reported
  `Time: 00:19.098`, `wordpress_phpunit_shell_real_seconds=29.647`, and
  `wordpress_total_seconds=31`; no
  `wordpress_phpunit_child_process_*` profiling lines were emitted.
- A same-session A/B before the CI env change passed the same focused class
  with static scan disabled in both modes: child profiling disabled measured
  `wordpress_phpunit_shell_real_seconds=28.711`, while child profiling enabled
  measured `wordpress_phpunit_shell_real_seconds=30.354`.
- `git diff --check` passed.
