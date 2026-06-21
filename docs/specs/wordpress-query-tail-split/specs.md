# WordPress Query Tail Split

## Problem

The REST content-controller split moved the WordPress PHPUnit critical path to
the query shard. Green run `27911524826` on `cad3e0a8` reported:

- setup total: `68s`;
- critical shard: `non-isolated-query` at `89s`;
- query artifact download: `8s`;
- query artifact extract: `2s`;
- query Docker image setup: `35s`;
- query PHPUnit total: `44s`;
- query PHPUnit shell real: `43.721s`;
- estimated WordPress workflow critical path: `157s`.

The same run reported `phpunit-non-isolated-rest-controller-other` as the
largest PHPUnit shell body at `64.146s`, but the query shard controls the
wall-clock critical path because its fixed shard setup was high. Splitting the
query shard is the bounded next step because it can reduce both the critical
query body and the chance that one large query shard receives all fixed setup
overhead.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_RE` as the visible query shard
  union and keeps `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` as
  the broader remaining-shard exclusion authority.
- A local static test-method proxy over the pinned WordPress sources found the
  current query bucket contains `540` test methods across `23` classes:
  - filter-heavy query classes: `291` test methods, `7` classes, selecting
    `Conditionals|DateQuery|MetaQuery|Results|Search|SearchColumns|TaxQuery`;
  - core/cache/status/setup query classes: `249` test methods, `16` classes,
    selecting `Test_Query_CacheResults`, the root `Tests_Query`, and
    `CommentCount|CommentFeed|Date|FieldsClause|GeneratePostdata|InvalidQueries|IsTerm|NoFoundRows|ParseQuery|PostStatus|SetupPostdata|Stickies|ThePost|Vars|VerbosePageRules`.

## Design

Replace `non-isolated-query` with two visible shards:

- `non-isolated-query-filter` / `phpunit-non-isolated-query-filter`,
  selecting query condition, date-query, meta-query, result, search, and
  tax-query classes;
- `non-isolated-query-core` / `phpunit-non-isolated-query-core`, selecting the
  root, cache, comment, date, field, generate-postdata, invalid-query,
  post-status, setup-postdata, sticky, post-loop, vars, and verbose-rule query
  classes.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_RE` as the documented union
of those two sub-regexes and keep
`MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` as the remaining-shard
exclusion authority. The split changes only visible matrix jobs and test-only
filter case arms.

Both shards keep the same production test-only settings as the old combined
query shard: Release MyLite PHP artifacts, MinSizeRel MariaDB embedded
archive, runtime manifest verification, restored prepared baseline database,
keepalive enabled, parent install skip enabled, child install skip disabled,
default PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting individual WordPress classes by method name.
- Changing the canonical, theme, remaining, or query/theme exclusion
  boundaries.
- Optimizing Docker image setup, artifact transfer, or native open/close in
  this slice.

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
additional WordPress PHPUnit shard job. The expected benefit is a shorter
WordPress critical path and clearer query timing attribution, at the cost of
one additional parallel runner job for artifact download/extract and Docker
image reuse.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition proof over the pinned WordPress test-method
  proxy for the full non-isolated shard set.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the new query-filter and
  query-core shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-query-filter` and
  `phpunit-non-isolated-query-core` labels.
- The old visible `phpunit-non-isolated-query` matrix label and case arm are
  rejected by the production-build audit.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_RE` remains present
  and equals the union of the filter and core sub-regexes in the partition
  proof.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` remains the
  remaining-shard exclusion authority.
- Focused PCRE samples show pinned WordPress test-method proxy names match
  exactly one non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `cad3e0a8` plus this slice's edits.

Passed:

- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress static
  test-method proxy and reported:
  - `rest-content-post`: `609` test methods, `6` classes;
  - `rest-content-support`: `361` test methods, `7` classes;
  - `rest-controller-other`: `897` test methods, `31` classes;
  - `rest-other`: `447` test methods, `9` classes;
  - `query-filter`: `291` test methods, `7` classes;
  - `query-core`: `249` test methods, `16` classes;
  - `canonical`: `26` test methods, `9` classes;
  - `theme`: `316` test methods, `17` classes;
  - `block-token`: `579` test methods, `67` classes;
  - `media-comment`: `991` test methods, `94` classes;
  - `content-entity`: `1200` test methods, `98` classes;
  - `content-settings-meta`: `422` test methods, `34` classes;
  - `user-auth`: `646` test methods, `41` classes;
  - `remaining-platform`: `1775` test methods, `159` classes;
  - `remaining-admin-site`: `1656` test methods, `217` classes;
  - `remaining-other`: `1719` test methods, `270` classes;
  - `overlap_count=0`;
  - `query_union_missing=0 extra=0 combined=540 split=540`.
- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for
`phpunit-non-isolated-query-filter` and `phpunit-non-isolated-query-core`.

## Risks

- Static test-method counts do not expand data-provider cases. CI timing
  remains the authority for the actual wall-clock split.
- Adding one matrix job increases total runner work. The current timing data
  shows query is the wall-clock critical shard, so this is acceptable for a
  visibility and wall-clock slice.
