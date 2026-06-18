# WordPress PHPUnit Deferred Child Install Split

## Problem Statement

The eager process-isolated WordPress PHPUnit shard now skips WordPress install
inside child processes, which removes a repeated setup block from every child.
The deferred-reconnect shard could not enable the same flag as one broad group:
focused verification showed failures in `Tests_Admin_ExportWp` and
`Tests_Sitemaps_Sitemaps` when `WP_TESTS_SKIP_INSTALL=1` was injected into
children.

The failed classes should keep the normal child install behavior, but the rest
of the deferred-reconnect shard should not continue paying the install cost
only because it shared one CI filter.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `tools/wordpress-phpunit-mysqli-mylite` already supports
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`, which injects
  `WP_TESTS_SKIP_INSTALL=1` only into PHPUnit child environments.
- The prior full deferred-reconnect filter passed with child install skip
  disabled and failed with child install skip enabled. The observed failures
  came from `Tests_Admin_ExportWp` and
  `Tests_Sitemaps_Sitemaps::test_disable_sitemap_should_return_404`.
- The deferred filter also contains `Tests_oEmbed_HTTP_Headers`, one
  filesystem chmod method, and three `Tests_Functions_WpUniquePrefixedId`
  methods that do not need the failing child-install semantics in the prior
  negative run.

## Scope And Non-Goals

In scope:

- Split the deferred-reconnect CI filter into install-required and
  child-install-skip-safe subsets.
- Add a new deferred-reconnect skip-install CI step with
  `MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0` and
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`.
- Keep `Tests_Admin_ExportWp` and the sitemap methods on the install-required
  deferred-reconnect step with child install skip disabled.
- Extend the CI production-build audit so this split cannot silently collapse
  or move child install skip onto the wrong shard.

Out of scope:

- Changing product SQL/runtime behavior, WordPress source, PHPUnit annotations,
  or database preparation semantics.
- Retrying the failing deferred classes with altered WordPress fixtures.
- Enabling child-process or mysqli profiling in normal CI timing steps.

## Design

The install-required deferred filter remains under
`MYLITE_WORDPRESS_PHPUNIT_DEFERRED_RECONNECT_FILTER`:

```text
^Tests_Admin_ExportWp(::|$)|Tests_Sitemaps_Sitemaps::test_disable_sitemap_should_return_404|Tests_Sitemaps_Sitemaps::test_empty_url_list_should_return_404
```

The child-install-skip-safe deferred filter is introduced as
`MYLITE_WORDPRESS_PHPUNIT_DEFERRED_RECONNECT_SKIP_INSTALL_FILTER`:

```text
^Tests_oEmbed_HTTP_Headers(::|$)|Tests_Filesystem_WpFilesystemDirect_Chmod::test_should_handle_set_mode_when_not_passed|Tests_Functions_WpUniquePrefixedId::test_should_create_unique_prefixed_ids|Tests_Functions_WpUniquePrefixedId::test_should_raise_notice_and_use_empty_string_prefix_when_nonstring_given|Tests_Functions_WpUniquePrefixedId::test_same_prefixes_should_generate_unique_ids
```

Both steps keep deferred reconnect behavior with
`MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0`. Only the skip-safe step
sets `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1`.

## Compatibility Impact

No product compatibility surface changes. The change affects only CI timing
layout for WordPress PHPUnit process-isolated tests.

## Directory And Lifecycle Impact

No durable directory-layout changes. The install-required shard keeps the
existing child install behavior; the skip-safe shard reuses the prepared parent
WordPress test database.

## Build, Size, License, And Dependencies

No compiled-code, binary-size, license, or dependency changes. The slice edits
the CI workflow, the static CI audit, and documentation.

## Verification Plan

- Run `bash -n tools/check-ci-production-builds
  tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run focused production WordPress PHPUnit for the install-required deferred
  filter with child install skip disabled.
- Run focused production WordPress PHPUnit for the skip-safe deferred filter
  with child install skip enabled.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Completed on 2026-06-18 with the pinned CI WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, the guarded
`build/wordpress-php-embedded-prod` Release build, the
`build/wordpress-mariadb-embedded` MinSizeRel archive, and an external
`/tmp`-backed MyLite WordPress test directory.

Syntax and CI audit checks passed:

```text
bash -n tools/check-ci-production-builds tools/wordpress-phpunit-mysqli-mylite
tools/check-ci-production-builds
ci_production_build_audit_ok=.github/workflows/ci.yml

cmake --preset prod
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
tools.ci-production-builds ... Passed
```

The pinned WordPress ref was refreshed and the prepared database was rebuilt:

```text
MYLITE_WORDPRESS_REF=6ddfc9d9b532c6e95c1266165149815895e2eb56 \
MYLITE_WORDPRESS_PHASE=fetch \
tools/wordpress-phpunit-mysqli-mylite
wordpress_sha=6ddfc9d9b532c6e95c1266165149815895e2eb56

MYLITE_WORDPRESS_REF=6ddfc9d9b532c6e95c1266165149815895e2eb56 \
MYLITE_WORDPRESS_PHASE=prepare-db \
tools/wordpress-phpunit-mysqli-mylite
wordpress_prepare_db_seconds=1
```

The install-required deferred shard passed with child install skip disabled:

```text
MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0 \
MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=0 \
tools/wordpress-phpunit-mysqli-mylite \
  --filter '^Tests_Admin_ExportWp(::|$)|Tests_Sitemaps_Sitemaps::test_disable_sitemap_should_return_404|Tests_Sitemaps_Sitemaps::test_empty_url_list_should_return_404'

OK (13 tests, 37 assertions)
wordpress_phpunit_reported_seconds=97.503
wordpress_phpunit_shell_real_seconds=110.170
```

The deferred skip-safe shard passed with child install skip enabled:

```text
MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0 \
MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1 \
tools/wordpress-phpunit-mysqli-mylite \
  --filter '^Tests_oEmbed_HTTP_Headers(::|$)|Tests_Filesystem_WpFilesystemDirect_Chmod::test_should_handle_set_mode_when_not_passed|Tests_Functions_WpUniquePrefixedId::test_should_create_unique_prefixed_ids|Tests_Functions_WpUniquePrefixedId::test_should_raise_notice_and_use_empty_string_prefix_when_nonstring_given|Tests_Functions_WpUniquePrefixedId::test_same_prefixes_should_generate_unique_ids'

WARNINGS!
Tests: 18, Assertions: 44, Warnings: 5, Skipped: 1.
wordpress_phpunit_reported_seconds=42.089
wordpress_phpunit_shell_real_seconds=57.019
```

The split deferred shards reported `139.592s` of PHPUnit time in this sample
(`97.503s + 42.089s`), compared with the prior unsplit deferred-reconnect
sample of `164.933s` reported time with child install skip disabled.

Production formatting and whitespace checks passed:

```text
cmake --build --preset format-check-prod
git diff --check
```

## Acceptance Criteria

- CI exposes separate timing rows for install-required and child-install-skip
  deferred-reconnect process-isolated shards.
- The install-required deferred shard keeps child install skip disabled.
- The skip-safe deferred shard enables child install skip and passes.
- CI audit fails if the split collapses or if child install skip moves to the
  database, non-isolated, or install-required deferred shard.
- Normal production build guards and formatting checks pass.

## Risks And Follow-Up

- If WordPress changes future process-isolated tests or fixtures, the skip-safe
  filter may need retesting before widening.
- This reduces repeated child install work for only part of the deferred shard;
  remaining install-required tests still pay the proven necessary setup cost.
