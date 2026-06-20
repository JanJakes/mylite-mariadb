# WordPress PHPUnit content/user shard

## Problem

The ownerless branch now publishes WordPress PHPUnit timing from production
build artifacts, and the latest green CI timing summary for commit `6d7afdae`
showed the non-isolated WordPress test set was still uneven:

- `phpunit-non-isolated-rest`: `162.786s` shell real.
- `phpunit-non-isolated-query-theme-block-token`: `155.892s` shell real.
- `phpunit-non-isolated-remaining`: `263.471s` shell real.

That means the current critical timing path is still governed by the broad
remaining shard even though the process-isolated and database shards are much
smaller. This slice reduces that CI wall-clock bottleneck and keeps the
remaining timing data visible without changing the test harness runtime,
database semantics, or production build shape.

## Source Findings

- `.github/workflows/ci.yml` already separates setup, production artifact
  packing, database, process-isolated, and non-isolated WordPress PHPUnit
  shards.
- `tools/wordpress-phpunit-timing-rollup` already treats every
  `phpunit-non-isolated-*` label as part of the non-isolated aggregate, so new
  non-isolated shards are included in the existing rollup without tool changes.
- `tools/check-ci-production-builds` guards the WordPress timing job against
  accidental developer builds, collapsed PHPUnit phases, disabled production
  guards, and missing timing artifacts.

## Design

Add a fourth non-isolated shard named `non-isolated-content-user` with label
`phpunit-non-isolated-content-user`. It selects WordPress content, user,
customize, template, XML-RPC, option, and metadata class families from the same
restored non-isolated base filter used by the existing shards.

The old `non-isolated-remaining` shard now excludes REST, query/theme/block,
and content/user families. All non-isolated shards keep:

- `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
- production `Release` MyLite artifact guards,
- production `MinSizeRel` MariaDB embedded archive guards,
- `MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1`,
- `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`.

## Non-Goals

- Do not change WordPress PHPUnit coverage.
- Do not change MyLite SQL execution, ownerless storage behavior, or durability.
- Do not enable diagnostic JUnit logging or mysqli profiling on the critical
  timing path.
- Do not claim an engine throughput improvement; this is CI wall-clock and
  visibility work.

## Compatibility Impact

No public API, SQL, database-directory, or native storage behavior changes. The
slice only changes how CI partitions already-selected WordPress PHPUnit tests.

## Test and Verification Plan

- `bash -n tools/check-ci-production-builds`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$' --output-on-failure`
- `git diff --check`

## Acceptance Criteria

- CI has a distinct `phpunit-non-isolated-content-user` timing label.
- The `phpunit-non-isolated-remaining` filter excludes the new class-family
  shard.
- The production-build audit fails if the new shard or its remaining-shard
  exclusion is removed.
- Existing timing rollups still aggregate every `phpunit-non-isolated-*` label.
