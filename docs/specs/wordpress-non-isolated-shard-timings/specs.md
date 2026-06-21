# WordPress Non-Isolated Shard Timings

## Problem

The WordPress CI job now separates setup, build, perf probe, process-isolated
PHPUnit, and the remaining non-isolated PHPUnit suite. The latest green run
before this slice completed the whole WordPress job in about `19m45s`, but the
single non-isolated remaining step still reported `661.843s` shell real and
`658.629s` PHPUnit time. That step is too large to tell which broad class
family should be optimized next.

## Source Findings

- CI run `27752540711` on `0687a30e` showed:
  - `build-php`: `322s`;
  - `phpunit-db`: `8.571s` shell real;
  - `phpunit-deferred-reconnect-install-required`: `57.992s`;
  - `phpunit-deferred-reconnect-skip-install`: `20.622s`;
  - `phpunit-eager-reconnect`: `32.990s`;
  - `phpunit-non-isolated`: `661.843s` shell real and `658.629s` reported.
- The non-isolated step already runs with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`, `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`,
  production build guards, and no default JUnit logging. The easy repeated
  startup lever is therefore already in use for this shard.
- An older local JUnit report with 29,246 tests and 1,326.565s total recorded
  broad class-family timing:
  - REST class prefixes `Tests_REST`, `WP_Test_REST`, and `WP_REST`: 3,234
    tests and 330.203s;
  - query/theme/block/token prefixes `Tests_Query`, `Test_Query`,
    `Tests_Canonical`, `Tests_Theme`, `Tests_WpTokenMap`, `Tests_Block`,
    `WP_Block`, and `Tests_Blocks`: 8,436 tests and 380.578s;
  - all other class prefixes: 17,576 tests and 615.784s.

## Design

Keep the existing non-isolated exclusion filter as the coverage authority and
derive three visible timing filters from it:

- REST shard: positive lookahead for
  `Tests_REST|WP_Test_REST|WP_REST`, then the existing non-isolated exclusions.
- Query/theme/block/token shard: positive lookahead for
  `Tests_Query|Test_Query|Tests_Canonical|Tests_Theme|Tests_WpTokenMap|Tests_Block|WP_Block|Tests_Blocks`,
  then the existing non-isolated exclusions.
- Remaining shard: contains-style negative lookahead for both positive-prefix
  groups, then the existing non-isolated exclusions.

This keeps the split as a partition of the previous non-isolated step while
preserving the dedicated `Tests_DB*` shard and the process-isolated class and
method exclusions. A later production timing slice further split the REST
partition into `Controller` and non-`Controller` filters after the REST shard
became the longest single PHPUnit gate. A follow-up split then separated the
query/theme/canonical classes from block/token classes after
`phpunit-non-isolated-query-theme-block-token` became the longest shard. Run
`27895890622` later showed the broad content/user shard as the longest
remaining test-only gate at `139.988s` shell real, ahead of REST controller at
`127.890s` and remaining at `115.693s`, so the next split divides content/media
classes from user/auth/privacy classes while preserving the same non-isolated
exclusion authority. Run `27896266558` then showed
`phpunit-non-isolated-rest-controller` as the longest test-only gate at
`147.060s` shell real, ahead of `phpunit-non-isolated-remaining` at
`122.608s` and `phpunit-non-isolated-content-media` at `117.366s`, so the
next split divides REST content-object controllers from the remaining REST
controllers. Run `27896697422` then showed the REST split succeeded:
`phpunit-non-isolated-rest-content-controller` reported `71.608s`,
`phpunit-non-isolated-rest-controller-other` reported `58.761s`, and the
new tail moved to `phpunit-non-isolated-remaining` at `121.495s` with
`phpunit-non-isolated-content-media` close behind at `118.110s`. The next
timing fanout therefore divides both broad tails while preserving the same
non-isolated exclusion authority.

The REST shard includes
`WP_Test_REST_Schema_Initialization::test_build_wp_api_client_fixtures`, which
dispatches oEmbed responses and reads WordPress' generated
`src/wp-includes/js/wp-embed.js` asset. The MyLite PHP-only WordPress harness
does not run the full npm/grunt source build, so the fetch/dependency phases
stage that single asset from WordPress' own `src/js/_enqueues/wp/embed.js`
source mapping before test-only phases run.

## Compatibility Impact

No SQL, C API, PHP API, storage, or WordPress behavior changes. CI still runs
the same WordPress tests; only the non-isolated timing boundary changes.

## Build And CI Impact

The WordPress job gains two extra Docker container invocations and two extra
baseline restores. Baseline restore was `0s` in the latest green run, and the
per-step PHPUnit shell overhead was about `3s`, so the expected wall-time
increase is small. The benefit is that future CI summaries show which broad
non-isolated shard dominates before any heavier sharding or query-path
optimization is attempted.

The production-build audit now requires all three non-isolated steps to keep
the Release/MinSizeRel guards, `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`,
`MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`, and disabled mysqli profiling.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE sample for the REST, query/theme/block/token, and
  remaining shard filters.
- Run a focused `MYLITE_WORDPRESS_PHASE=fetch` check and confirm the
  WordPress embed asset is staged for REST schema fixture generation.
- Run `git diff --check`.
- Run a focused YAML/script inspection to confirm the three filters derive
  from `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER`.
- Let CI report the first real shard timings, since the full non-isolated
  suite is too slow to duplicate locally for this workflow-only slice.

## Verification Results

Follow-up verification on 2026-06-18 after the first REST-shard CI run failed
on a missing `src/wp-includes/js/wp-embed.js` asset:

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `bash -n tools/check-ci-production-builds` passed.
- `tools/check-ci-production-builds` passed.
- A focused PHP PCRE sample confirmed
  `WP_Test_REST_Schema_Initialization::test_build_wp_api_client_fixtures`,
  `Tests_REST*`, and contains-style `WP_REST` names match only the REST shard;
  `Tests_Query*` and `Tests_Block*` names match only the query/theme/block/token
  shard; ordinary names match only the remaining shard; and `Tests_DB` plus the
  process-isolated exclusions match none of the non-isolated shards.
- A focused `MYLITE_WORDPRESS_PHASE=fetch` run against WordPress ref
  `6ddfc9d9b532c6e95c1266165149815895e2eb56` passed with
  `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1` and staged
  `src/wp-includes/js/wp-embed.js`.
- A local `cmp` confirmed the staged `src/wp-includes/js/wp-embed.js` matches
  WordPress' `src/js/_enqueues/wp/embed.js` source file.
- `git diff --check` passed.

## Risks And Follow-Up

The split currently improves timing visibility, not total CI wall time. If the
new shard data is stable, the next workflow optimization is either a parallel
WordPress PHPUnit matrix with artifact handoff or a narrower code-level
optimization for the hottest shard. Parallelization should not duplicate the
full MariaDB/PHP extension build unless the saved test time clearly outweighs
that cost.
