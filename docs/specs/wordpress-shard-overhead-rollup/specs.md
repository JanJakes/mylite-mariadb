# WordPress Shard Overhead Rollup

## Problem

The WordPress PHPUnit job now splits test execution across many production
shards. That made the slowest PHPUnit body smaller, but the latest green run
still showed a wide gap between the critical shard total and its PHPUnit shell
time. Without a first-class rollup row, each CI run requires manual arithmetic
to separate MyLite/PHPUnit work from artifact download, artifact extraction,
Docker image setup, and harness wrapping.

This slice makes that fixed-cost breakdown visible in the published timing
summary so performance work can target the remaining wall-clock cost instead
of blindly creating more shards.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` publishes per-phase WordPress timing rows for
  setup, shard artifact download, shard artifact extraction, per-shard Docker
  image build, PHPUnit execution, and the final rollup.
- `tools/wordpress-phpunit-timing-rollup` already computes the shard critical
  path from rows named `artifact-download-*`, `artifact-extract-*`,
  `docker-image-*`, and `phpunit-*`, but it previously emitted only the
  combined critical-shard total.
- `tools/wordpress-phpunit-timing-rollup-test` has a synthetic timing fixture
  with two shard-shaped paths, making it the right regression point for the
  decomposition.
- The green production run for `53267ee3` reported
  `wordpress_shard_critical_path_label=non-isolated-query-canonical`,
  `wordpress_shard_critical_path_seconds_max=124`, and
  `wordpress_phpunit_shell_real_seconds_max=75.965`. The same existing rows
  imply about `48.035s` of non-PHPUnit-shell cost on that critical path.

## Design

Extend the rollup script to retain per-shard phase totals while it computes the
existing critical path:

- artifact download seconds,
- artifact extract seconds,
- Docker image seconds,
- PHPUnit total seconds,
- PHPUnit shell real seconds,
- non-PHPUnit-shell seconds, computed as shard critical-path seconds minus the
  matching PHPUnit shell real seconds.

Also emit the shard with the largest non-PHPUnit-shell cost. Keep the existing
metric names and critical-path calculation unchanged so prior dashboards remain
compatible.

`tools/check-ci-production-builds` requires the new metric names in the rollup
tool so a future workflow or tool edit cannot accidentally drop the overhead
breakdown while leaving the rest of the timing artifact intact.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, storage-engine, metadata, wire-protocol, or
directory-lifecycle behavior changes. This is a CI/reporting-only slice.

## Directory And Lifecycle Impact

No durable directory-layout changes. The WordPress harness still extracts and
uses the same runtime and database-baseline artifacts.

## Build And Performance Impact

The rollup adds AWK array bookkeeping over the existing timing summary rows.
The cost is outside production builds, PHP extension execution, and PHPUnit
test execution.

The new rows are intended to show when the next highest-impact optimization is
setup overhead, such as Docker/image or artifact handling, rather than MyLite
engine execution.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest wrappers for
  `tools.wordpress-phpunit-timing-rollup` and `tools.ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- The published rollup includes critical-shard phase rows for artifact
  download, artifact extraction, Docker image setup, PHPUnit total time,
  PHPUnit shell time, and non-PHPUnit-shell time.
- The rollup fixture proves the decomposition on the current synthetic
  critical path.
- The CI production audit guards the new rollup metric names.
- No compatibility or runtime behavior claim changes.

## Verification

- `bash -n tools/wordpress-phpunit-timing-rollup`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/wordpress-phpunit-timing-rollup-test`: passed.
- `bash -n tools/check-ci-production-builds`: passed.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed two tests.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.
