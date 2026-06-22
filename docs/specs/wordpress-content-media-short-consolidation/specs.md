# WordPress Content Media Short Consolidation

## Problem

The WordPress PHPUnit CI matrix is now production-built, test-only, and highly
sharded. The latest green ownerless-concurrency run `27944957498` on
`33c170dfd` shows the current critical path is no longer a monolithic PHPUnit
step:

- setup phases total `74s`;
- critical shard: `non-isolated-user-auth` at `72s`, with `38s` PHPUnit total;
- estimated WordPress workflow critical path: `146s`;
- total shard Docker setup rows: `620s`;
- total artifact download rows: `78s`;
- total artifact extract rows: `58s`;
- total PHPUnit shell time: `722.045s`.

The run also shows several short non-isolated shards still pay a separate
artifact download, artifact extract, and Docker image materialization. Two of
the shortest compatible standalone shards are:

- `phpunit-non-isolated-content-settings-meta`: `13.739s` shell real,
  `33s` total;
- `phpunit-non-isolated-media`: `15.524s` shell real, `34s` total.

Keeping these as separate matrix jobs spends roughly one extra shard setup
payment for about `29.263s` of combined PHPUnit shell time. Grouping them keeps
the projected grouped shard comfortably below the current `72s` critical tail
while reducing runner work and Docker/cache variance.

The latest `main` run available through GitHub Actions has no modern WordPress
timing artifact, so current branch timing artifacts are the authoritative
source for this scheduling slice.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- Both target shards use the same non-isolated WordPress settings:
  reconnect after child enabled, child timing disabled, child script timing
  disabled, mysqli profiling disabled, keepalive enabled, child install skip
  disabled, and baseline restore disabled.
- `.github/workflows/ci.yml` already supports grouped short shards through
  `run_wordpress_phpunit_filter` and `record_wordpress_phpunit_group`.
- `tools/wordpress-phpunit-timing-rollup` uses `group-phpunit-<shard>` rows
  for grouped-shard critical-path math while excluding those synthetic rows
  from aggregate PHPUnit sums.
- `docs/specs/wordpress-short-shard-consolidation/specs.md` established the
  existing grouping pattern for short same-mode shards.

## Design

Replace two standalone matrix entries with one grouped entry:

- `non-isolated-content-media-short` /
  `phpunit-non-isolated-content-media-short`.

The grouped job runs the existing child filters sequentially through separate
harness invocations with their original timing labels:

- `phpunit-non-isolated-content-settings-meta`;
- `phpunit-non-isolated-media`.

After both child filters pass, the workflow emits
`group-phpunit-non-isolated-content-media-short` with the grouped shell real
seconds so the rollup can attribute shard critical-path time without
double-counting the child PHPUnit rows.

The broad content-data and media/comment exclusion authorities remain
unchanged. This is only a matrix scheduling change.

## Non-Goals

- Changing WordPress test filters, WordPress source, or test coverage.
- Grouping process-isolated shards, database profile shards, or current
  critical-path shards.
- Changing Docker image contents, runtime artifacts, production build types, or
  MyLite runtime behavior.
- Claiming an engine-performance improvement.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout,
ownerless-concurrency, or WordPress application behavior changes. CI still runs
the same WordPress test filters with the same production MyLite and MariaDB
artifacts.

## Build And Performance Impact

The matrix loses one net WordPress shard job. Based on CI run `27944957498`,
this removes one repeated shard setup payment while grouping about `29.263s`
of child PHPUnit shell time. Actual wall-clock impact depends on GitHub runner
parallelism and Docker/cache variance. The grouped shard is expected to remain
below the current `72s` critical path.

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
- Let the next pushed CI run provide the full production timing for the
  grouped shard layout.

## Acceptance Criteria

- The workflow has one grouped content/media short-shard matrix entry and no
  standalone matrix entries for `non-isolated-content-settings-meta` or
  `non-isolated-media`.
- The grouped child filters still emit their original `phpunit-*` timing
  labels.
- The workflow emits a grouped timing row for
  `non-isolated-content-media-short`.
- The production-build audit requires the grouped matrix entry, child labels,
  and grouped case arm.
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

- Grouped jobs run child filters sequentially on the same GitHub runner. The
  selected filters are short non-isolated shards with identical production
  settings and separate harness invocations.
- The change reduces aggregate runner work more than it reduces wall-clock
  critical path. Larger gains would require reducing per-shard Docker image
  materialization or moving the current `user-auth` tail.
