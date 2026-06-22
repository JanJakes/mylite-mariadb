# WordPress Block Rest Short Consolidation

## Problem

The WordPress PHPUnit CI matrix now uses production builds and separate
test-only shard steps. CI run `27946196793` on `f5c3a527a` passed with the
new content/media short shard, but the timing rollup shows shard
materialization remains the dominant repeated cost:

- `wordpress_shard_phase_count`: `30`;
- `wordpress_setup_total_seconds_sum`: `76s`;
- `wordpress_shard_critical_path_label`: `non-isolated-canonical`;
- `wordpress_shard_critical_path_seconds_max`: `77s`;
- `wordpress_docker_build_seconds`: `704s`;
- `wordpress_artifact_download_seconds`: `107s`;
- `wordpress_artifact_extract_seconds`: `55s`.

The prior slice reduced one matrix job and kept
`non-isolated-content-media-short` to `58s`, but Docker/cache variance still
dominates the workflow. The next safe lever is to consolidate more short
same-mode non-isolated shards while preserving original child timing labels.

Two natural pairs are short enough to group without approaching the current
critical tail:

- `non-isolated-block-library`: `39s` total with `16s` PHPUnit;
- `non-isolated-block-supports`: `37s` total with `13s` PHPUnit;
- `non-isolated-rest-controller-wp-test`: `48s` total with `16s` PHPUnit;
- `non-isolated-rest-controller-wp-rest`: `39s` total with `14s` PHPUnit.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- All four target shards use the same non-isolated WordPress shard settings:
  reconnect after child enabled, child timing disabled, child script timing
  disabled, mysqli profiling disabled, keepalive enabled, child install skip
  disabled, and baseline restore disabled.
- `.github/workflows/ci.yml` already runs grouped shards via
  `run_wordpress_phpunit_filter` and emits synthetic group timing rows through
  `record_wordpress_phpunit_group`.
- `tools/wordpress-phpunit-timing-rollup` uses `group-phpunit-<shard>` rows
  for critical-path math while keeping child PHPUnit rows in aggregate PHPUnit
  sums.
- `docs/specs/wordpress-content-media-short-consolidation/specs.md` records
  the same grouping pattern and CI timing basis.

## Design

Replace four standalone matrix entries with two grouped entries:

- `non-isolated-block-library-supports-short` /
  `phpunit-non-isolated-block-library-supports-short`;
- `non-isolated-rest-controller-wp-short` /
  `phpunit-non-isolated-rest-controller-wp-short`.

The block group runs the existing child filters sequentially with their
original labels:

- `phpunit-non-isolated-block-library`;
- `phpunit-non-isolated-block-supports`.

The REST controller group runs the existing child filters sequentially with
their original labels:

- `phpunit-non-isolated-rest-controller-wp-test`;
- `phpunit-non-isolated-rest-controller-wp-rest`.

Each group emits a `group-phpunit-*` row for shard critical-path accounting.
The regex authorities and exclusions remain unchanged.

## Non-Goals

- Changing WordPress test filters or coverage.
- Grouping process-isolated shards, profile shards, or current critical-path
  shards.
- Changing Docker image contents, runtime artifact contents, production build
  modes, PHP extension behavior, or MyLite runtime behavior.
- Claiming an engine-performance improvement.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout,
ownerless-concurrency, or WordPress application behavior changes. CI still runs
the same WordPress test filters against the same production MyLite and MariaDB
artifacts.

## Build And Performance Impact

The matrix loses two net WordPress shard jobs. Based on CI run `27946196793`,
this should remove two repeated shard materialization payments while keeping
both grouped jobs below the current `77s` critical tail. Actual wall-clock
impact depends on GitHub runner scheduling and Docker/cache variance.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/check-ci-production-builds`.
- Run focused production CTest coverage for the CI production-build audit and
  WordPress timing rollup.
- Run the production format check.
- Run `git diff --check`.
- Let the next pushed CI run provide full production timing for the grouped
  shard layout.

## Acceptance Criteria

- The workflow has grouped entries for block library/supports and REST
  controller WP test/rest, with no standalone matrix entries or case arms for
  the four old shards.
- The grouped child filters still emit their original `phpunit-*` timing
  labels.
- The workflow emits grouped timing rows for both new shard names.
- The production-build audit requires the grouped matrix entries, child labels,
  grouped case arms, and production build guards.
- CI still uses Release MyLite PHP builds and a MinSizeRel MariaDB embedded
  archive for WordPress timings.

## Verification Results

- `bash -n tools/check-ci-production-builds`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Risks And Follow-Up

- Grouped jobs run child filters sequentially on one runner. The selected
  filters are short same-mode non-isolated shards and each child still uses a
  separate harness invocation.
- The improvement reduces repeated runner work more reliably than it reduces
  wall-clock critical path. Further wall-clock gains likely require reducing
  per-shard Docker image materialization or moving the current canonical/content
  tail.
