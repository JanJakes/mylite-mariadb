# WordPress Query And Block Shard Split

## Problem

After `c6855a84` split the REST PHPUnit shard, the green production CI run
`27885964945` moved the critical path to
`phpunit-non-isolated-query-theme-block-token`:

- `phpunit-non-isolated-query-theme-block-token`: `152.696s` shell real,
- `phpunit-non-isolated-content-user`: `145.002s`,
- `phpunit-non-isolated-rest-controller`: `127.738s`,
- `phpunit-non-isolated-remaining`: `119.282s`,
- `phpunit-non-isolated-rest-other`: `28.905s`.

The same run remained green under production build guards. The next bounded CI
performance target is therefore the broad query/theme/block/token shard.

## Source Findings

- `.github/workflows/ci.yml` previously selected the shard with one regex:
  `Tests_Query|Test_Query|Tests_Canonical|Tests_Theme|Tests_WpTokenMap|Tests_Block|WP_Block|Tests_Blocks`.
- `tools/wordpress-phpunit-timing-rollup` aggregates labels matching
  `phpunit-non-isolated-*`, so additional non-isolated labels stay in the
  existing rollup without tool changes.
- A local scan of the pinned WordPress test classes under the current base
  non-isolated filter found a clean split:
  `52` query/theme/canonical classes, `68` block/token classes, and no overlap
  across the proposed non-isolated partitions.

## Design

Replace `phpunit-non-isolated-query-theme-block-token` with two matrix entries:

- `phpunit-non-isolated-query-theme`, selected by
  `Tests_Query|Test_Query|Tests_Canonical|Tests_Theme`.
- `phpunit-non-isolated-block-token`, selected by
  `Tests_WpTokenMap|Tests_Block|WP_Block|Tests_Blocks`.

The remaining non-isolated shard excludes both new regexes, the REST regex,
and the content/user regex. Both new shards keep the same production `Release`
MyLite guard, MariaDB embedded `MinSizeRel` guard, restored baseline database,
`MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
`MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`, disabled mysqli profiling, and
disabled default PHPUnit logging.

## Compatibility Impact

No SQL, PHP API, public C API, storage, or WordPress compatibility behavior
changes. This only partitions already-selected WordPress PHPUnit tests.

## Directory And Lifecycle Impact

No product directory changes. Each shard still restores a transient MyLite
WordPress database under `/tmp` from the setup job's baseline artifact.

## Build And Performance Impact

No compiled code changes. The matrix gains one additional shard job. The goal
is to move the critical path below the current `152.696s` query/theme/block
bucket while preserving the same aggregate test coverage and timing rollups.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE sample against pinned WordPress class names and prove
  the REST, query/theme, block/token, content/user, and remaining filters are
  mutually exclusive under the base non-isolated filter.
- Run `ctest --preset prod -R '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$' --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI collect the first production timings for the split query/block shards.

## Acceptance Criteria

- CI has distinct `phpunit-non-isolated-query-theme` and
  `phpunit-non-isolated-block-token` timing labels.
- The production-build audit fails if either new shard or remaining-shard
  exclusion is removed.
- The final timing rollup still includes both labels in the non-isolated
  aggregate.

## Risks And Follow-Up

This reduces CI wall-clock exposure, not engine execution cost. If the next CI
run shows content/user or one of the new shards still dominates, use that
production timing artifact to choose the next split or runtime profile target.
