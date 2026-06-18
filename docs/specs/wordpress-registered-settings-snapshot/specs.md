# WordPress Registered Settings Snapshot

## Problem

The pinned WordPress PHPUnit ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56` contains
`Tests_Connectors_WpRegisterDefaultConnectorSettings`, which stores the lazy
`$wp_registered_settings` global in a non-nullable `array` property during
`set_up()`. A focused production-build run can fail before exercising MyLite:

```text
TypeError: Cannot assign null to property
Tests_Connectors_WpRegisterDefaultConnectorSettings::$original_registered_settings
of type array
```

This makes the split WordPress PHPUnit timing job look unstable even when the
failure is upstream test fixture state rather than MyLite SQL behavior.

## Source Findings

- `tests/phpunit/tests/connectors/wpRegisterDefaultConnectorSettings.php` line
  28 assigns the global `$wp_registered_settings` directly to a typed `array`
  property.
- `src/wp-includes/option.php` initializes that global lazily in
  `register_setting()`, while `get_registered_settings()` explicitly returns an
  empty array when the global has not yet been initialized.
- The local harness already stages WordPress PHPUnit source artifacts after each
  fetch so generated or patched test assets are deterministic before PHPUnit
  phases run.

## Design

During `stage_wordpress_phpunit_source_artifacts()`, patch the pinned WordPress
connector test once by replacing the direct global snapshot with
`get_registered_settings()`. The marker
`MYLITE_WORDPRESS_REGISTERED_SETTINGS_SNAPSHOT` makes the patch idempotent.

The patch is intentionally limited to the downloaded WordPress test checkout.
It does not change MyLite runtime code, SQL behavior, native storage, database
directory layout, or public APIs.

## Compatibility Impact

This is a WordPress PHPUnit harness compatibility fix. It preserves WordPress'
documented test intent: snapshot the currently registered settings before the
connector test mutates them. The API helper returns the same settings array when
initialized and a stable empty array when not initialized.

## Test Plan

- Run the focused connector PHPUnit filter against the pinned WordPress ref with
  production MyLite PHP artifacts and `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`.
- Run the dependencies phase to prove the source patch applies cleanly and
  remains idempotent.
- Run the CI production-build audit.
- Run shell syntax checks for the harness.

## Acceptance Criteria

- The focused connector filter passes from a baseline-restored WordPress test
  database.
- The harness prints either `wordpress_phpunit_connector_settings_patch=applied`
  or `present` during source staging.
- The production-build CI audit still passes.

## Risks

The fix is tied to the current WordPress test shape. If the upstream connector
test changes, the patcher fails closed with a clear error during fetch or
dependencies rather than silently running a partially patched suite.

## Verification

Validated against the pinned CI WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56` with
`MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod`,
`MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`, and
`MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1`:

- `MYLITE_WORDPRESS_PHASE=fetch ... tools/wordpress-phpunit-mysqli-mylite`
  applied the source patch and printed
  `wordpress_phpunit_connector_settings_patch=applied`.
- `MYLITE_WORDPRESS_PHASE=dependencies ... tools/wordpress-phpunit-mysqli-mylite`
  proved idempotence and printed
  `wordpress_phpunit_connector_settings_patch=present`.
- `MYLITE_WORDPRESS_PHASE=phpunit ... MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1
  tools/wordpress-phpunit-mysqli-mylite --filter
  '^Tests_Connectors_WpRegisterDefaultConnectorSettings(::|$)'` passed with
  `OK (2 tests, 2 assertions)`.
- `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- `tools/check-ci-production-builds`.
- `git diff --check`.
