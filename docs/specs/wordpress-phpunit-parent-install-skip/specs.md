# WordPress PHPUnit Parent Install Skip

## Problem

The WordPress PHPUnit CI job now separates build, database-prep, performance,
and test-only phases so timings are visible. The split also means each
test-only PHPUnit step can pay WordPress' parent bootstrap install cost again.
`tests/phpunit/includes/bootstrap.php` runs `install.php` unless
`WP_TESTS_SKIP_INSTALL=1`, while the MyLite harness already has a dedicated
`prepare-db` phase.

Child install skip removed duplicate install work inside verified
process-isolated child processes. It does not remove repeated parent
bootstrap install work from the split test-only steps.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- WordPress `tests/phpunit/includes/bootstrap.php` runs
  `tests/phpunit/includes/install.php <wp-tests-config.php> <ms-mode>
  <core-mode>` when `WP_TESTS_SKIP_INSTALL` is not `1`.
- The current MyLite `prepare-db` phase creates the MyLite database directory
  and writes `wp-tests-config.php`, but the parent PHPUnit bootstrap still does
  the actual WordPress table install for every test-only phase.
- The active WordPress MyLite test database is about 140 MiB in the current
  local production harness. Restoring a closed baseline directory from tmpfs is
  expected to be cheaper than repeating PHP-driven WordPress install work for
  every split test step.
- Existing child-process patching already distinguishes parent and child
  `WP_TESTS_SKIP_INSTALL` behavior through
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL`.

## Design

Add an opt-in harness flag:

```text
MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1
```

When enabled for a `phpunit` phase:

- require a prepared baseline database directory;
- restore the active `MYLITE_WORDPRESS_DB_DIR` from that baseline before
  starting PHPUnit;
- pass `WP_TESTS_SKIP_INSTALL=1` only to the parent PHPUnit process.

Extend `prepare-db` so it creates that baseline when the same flag is enabled:

1. Remove the active database directory and baseline directory.
2. Create the WordPress test database and write `wp-tests-config.php`.
3. Run WordPress' own `install.php` once through the production PHP wrapper.
4. Copy the clean, closed database directory to a sibling baseline directory.

With the flag disabled, `prepare-db` preserves the old all-in-one harness shape:
create the database directory and config, then let the parent PHPUnit bootstrap
run WordPress install. With the flag enabled, `prepare-db` creates the installed
baseline and later `phpunit` phases restore it before starting PHPUnit.

The default baseline path is `${MYLITE_WORDPRESS_DB_DIR}.baseline`. For the
external `/tmp` CI database placement this keeps active and baseline
directories under the same mounted parent. Custom baseline paths must be
separate from the active database path; neither directory may contain the
other.

Child processes remain separately controlled. If parent skip is enabled but
`MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=0`, the PHPUnit child patch forces
`WP_TESTS_SKIP_INSTALL=0` in the child environment so install-required child
tests keep the old behavior.

## Compatibility Impact

No MyLite SQL, C API, mysqli API, native storage, or product behavior changes.
This is a WordPress CI harness optimization.

The parent test process still starts from a clean installed WordPress database;
the difference is that the clean state comes from a prepared MyLite directory
copy rather than rerunning WordPress install in each split test step.

## Directory And Lifecycle Impact

The baseline directory is a temporary harness artifact next to the active
WordPress test database directory. It is not application state and is not part
of MyLite's durable database-directory contract.

The active database is closed before the baseline is copied and before each
restore. Test-only phases remove and replace only the configured active
WordPress test database directory.

## Build, Size, And Dependencies

No compiled-code, binary-size, dependency, or license impact. The change is
limited to the Bash/PHP WordPress harness, CI wiring, audit checks, and docs.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite` and
  `bash -n tools/check-ci-production-builds`.
- Run the production `prepare-db` phase and verify it emits install and
  baseline-copy timings.
- Run focused production `^Tests_DB` with parent install skip and verify the
  output no longer contains the parent `Installing...` line while tests pass.
- Run a focused process-isolated install-required test with parent skip enabled
  and child skip disabled to verify child install still runs.
- Run a focused process-isolated child-skip-safe test with both parent and
  child skip enabled to verify duplicate install work stays suppressed.
- Run `tools/check-ci-production-builds`, the production CTest audit,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Parent install skip is disabled unless explicitly requested.
- `prepare-db` creates a clean installed baseline when
  `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1` is used for split test-only phases.
- Parent PHPUnit skip does not force child install skip for install-required
  process-isolated tests.
- CI production timing steps keep Release/MinSizeRel guards and enable parent
  install skip only after the dedicated prepare-db step.
- Timing summaries include baseline preparation and restore metrics.

## Risks

The optimization assumes a clean installed WordPress test database can be
restored from a closed MyLite directory between split test-only phases. If a
future test shard requires a different parent bootstrap install mode, that
shard should keep `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=0` until it has
focused evidence.

## Verification Results

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `bash -n tools/check-ci-production-builds` passed.
- `MYLITE_WORDPRESS_PHASE=dependencies` refreshed the warmed PHPUnit vendor and
  the patched child-process launcher passed `php -l`.
- `MYLITE_WORDPRESS_PHASE=prepare-db
  MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1` created the active database,
  performed one WordPress install, copied the baseline, and emitted create,
  install, baseline-copy, and total timings.
- Focused production `^Tests_DB` passed from a restored baseline without the
  parent `Installing...` line (`651 tests, 3 skipped`).
- Focused install-required child smoke passed with parent skip enabled and
  child skip disabled (`1 test, 1 assertion`), preserving the child install
  path.
- Focused child-skip-safe process-isolated smoke passed with both parent and
  child skip enabled (`7 tests, 14 assertions`), with child runtime averaging
  about `1.7s` versus about `9.4s` for the install-required child smoke.
- Focused non-isolated keepalive smoke passed from the restored baseline
  (`2 tests, 3 assertions`).
