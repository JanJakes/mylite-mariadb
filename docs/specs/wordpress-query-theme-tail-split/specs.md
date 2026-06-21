# WordPress Query Theme Tail Split

## Problem

The top-tail fanout in CI run `27897052195` on `eec8f351` moved the WordPress
PHPUnit critical path to the remaining broad query/theme shard:

- `phpunit-non-isolated-query-theme`: `105.802s` shell real, `102.651s`
  reported, `107s` total;
- `phpunit-non-isolated-rest-content-controller`: `75.894s` shell real,
  `72.817s` reported, `76s` total;
- `phpunit-non-isolated-content-data`: `72.858s` shell real, `69.790s`
  reported, `74s` total;
- `phpunit-non-isolated-rest-controller-other`: `69.056s` shell real,
  `65.258s` reported, `70s` total.

The timing rollup identified `non-isolated-query-theme` as the shard critical
path at `147s`, with estimated WordPress workflow critical path at `229s`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` currently uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` as the combined
  query/theme exclusion group for remaining shards.
- A local scan of pinned WordPress test names found the query/theme bucket has
  `885` test names across `52` classes.
- Splitting the bucket into `Tests_Query|Test_Query|Tests_Canonical` and
  `Tests_Theme` yields:
  - `query-canonical`: `569` test names, `35` classes;
  - `theme`: `316` test names, `17` classes.

## Design

Replace `non-isolated-query-theme` with two visible shards:

- `non-isolated-query-canonical` /
  `phpunit-non-isolated-query-canonical`, selecting
  `Tests_Query|Test_Query|Tests_Canonical`;
- `non-isolated-theme` / `phpunit-non-isolated-theme`, selecting
  `Tests_Theme`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` as the broad
remaining-shard exclusion authority. The split introduces sub-regexes only for
the visible shard filters, avoiding churn in the already-proven
remaining/platform/admin-site/other negative lookaheads.

Both new shards keep the previous production test-only settings: Release
MyLite PHP artifacts, MinSizeRel MariaDB embedded archive, runtime manifest
verification, restored baseline database, keepalive enabled, parent install
skip enabled, child install skip disabled, default PHPUnit logging disabled,
and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting query classes by method name.
- Rebalancing the remaining, REST, block/token, content, or user/auth shards.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, storage, directory, native recovery, or
WordPress application behavior changes. CI still runs the same WordPress test
surface with the same production MyLite and MariaDB artifacts.

## Directory And Lifecycle Impact

No durable layout changes. Each shard continues to restore the same prepared
WordPress MyLite database baseline into the same external temporary parent
directory before its test-only phase.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, or ownerless concurrency
behavior changes.

## Build, Size, License, And Dependency Impact

No compiled-code, binary-size, license, or dependency changes. CI gains one
additional WordPress PHPUnit shard job. The goal is lower wall-clock critical
path and clearer timing attribution, at the cost of one additional parallel
runner job for artifact download/extract and Docker image reuse.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition proof over pinned WordPress test method
  names for the full non-isolated shard set.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the new query/canonical
  and theme shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-query-canonical` and
  `phpunit-non-isolated-theme` labels.
- The old `phpunit-non-isolated-query-theme` label is replaced by those two
  labels.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` remains the
  remaining-shard exclusion authority.
- The production audit fails if either new shard, regex, or case arm
  disappears.
- Focused PCRE samples show pinned WordPress test method names match exactly
  one non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `eec8f351` plus this slice's edits.

Passed:

- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked real pinned WordPress test method
  names and reported:
  - `rest-content-controller`: `970` test names, `13` classes;
  - `rest-controller-other`: `898` test names, `32` classes;
  - `rest-other`: `447` test names, `9` classes;
  - `query-canonical`: `569` test names, `35` classes;
  - `theme`: `316` test names, `17` classes;
  - `block-token`: `580` test names, `68` classes;
  - `media-comment`: `991` test names, `94` classes;
  - `content-data`: `1622` test names, `132` classes;
  - `user-auth`: `641` test names, `41` classes;
  - `remaining-platform`: `2011` test names, `161` classes;
  - `remaining-admin-site`: `1653` test names, `217` classes;
  - `remaining-other`: `1933` test names, `287` classes.
- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for
`phpunit-non-isolated-query-canonical` and `phpunit-non-isolated-theme`.

## Risks

- Test-name and class counts are only runtime proxies. CI timing remains the
  authority for whether query/canonical or theme controls the next tail.
- Adding one matrix job increases total runner work. The current timing data
  shows query/theme is the clear wall-clock tail, so this is acceptable for a
  visibility and wall-clock slice.
