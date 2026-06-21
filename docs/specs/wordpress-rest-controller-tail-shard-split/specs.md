# WordPress REST Controller Tail Shard Split

## Problem

The WordPress PHPUnit matrix already separates build/setup from production
test-only shards, but the latest green production run shows the remaining
critical PHPUnit tail is the broad REST controller bucket. CI run
`27896266558` on `5385dc28` reported:

- `phpunit-non-isolated-rest-controller`: `147.060s` shell real,
  `143.930s` reported, `147s` total;
- `phpunit-non-isolated-remaining`: `122.608s` shell real, `119.399s`
  reported, `124s` total;
- `phpunit-non-isolated-content-media`: `117.366s` shell real,
  `114.134s` reported, `118s` total;
- `phpunit-non-isolated-rest-other`: `28.061s` shell real, `24.716s`
  reported, `29s` total;
- the timing rollup identified `non-isolated-rest-controller` as the shard
  critical-path label at `186s`, with estimated workflow critical path at
  `260s`.

The REST controller bucket is now both the slowest PHPUnit shell-time shard
and the slowest shard total. It needs a second visible split before making
engine-level claims about remaining PHPUnit performance.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` derives non-isolated filters from
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER` and currently identifies REST
  tests through `Tests_REST|WP_Test_REST|WP_REST`.
- A local scan of the pinned WordPress tree under
  `build/wordpress-develop/tests/phpunit/tests` found `45` REST controller
  classes with about `1,867` `test_*` methods under the current production
  filter.
- The WordPress content-object REST controllers
  `WP_Test_REST_(Posts|Pages|Attachments|Comments|Users|Tags|Categories|Taxonomies|Revisions|Autosaves|Search|Post_Types|Post_Statuses)_Controller`
  account for `13` of those classes and about `970` `test_*` methods. The
  remaining REST controller classes account for `32` classes and about `897`
  `test_*` methods.
- `tools/check-ci-production-builds` is the local and CI guard for production
  build type, split test-only execution, timing summaries, keepalive, and
  shard filter boundaries.

## Design

Replace the single `non-isolated-rest-controller` matrix entry with two
entries:

- `non-isolated-rest-content-controller` /
  `phpunit-non-isolated-rest-content-controller`, selecting REST controller
  classes that match the WordPress content-object controller regex above;
- `non-isolated-rest-controller-other` /
  `phpunit-non-isolated-rest-controller-other`, selecting REST controller
  classes that do not match that regex.

The existing `non-isolated-rest-other` shard remains unchanged and continues
to select REST classes that do not contain `Controller`. The broad
`non-isolated-remaining` shard continues to exclude the full REST regex, so
this slice only changes the internal REST controller boundary.

Both new shards keep the previous production test-only settings: Release
MyLite PHP artifacts, MinSizeRel MariaDB embedded archive, runtime manifest
verification, restored baseline database, keepalive enabled, parent install
skip enabled, child install skip disabled, default PHPUnit logging disabled,
and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Enabling mysqli profiling on long non-isolated shards by default.
- Claiming the ownerless concurrency objective is complete.
- Splitting individual WordPress classes by method name.

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
additional WordPress PHPUnit shard invocation. The prior run showed per-shard
artifact download/extract and Docker image phases around tens of seconds, but
the matrix runs these shard jobs in parallel and the current REST controller
bucket controls the critical path.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition sample for DB, process-isolated exclusions,
  REST content controller, REST controller-other, REST non-controller,
  query/theme, block/token, content/media, user/auth, and remaining class
  names.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the two new REST
  controller shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-rest-content-controller` and
  `phpunit-non-isolated-rest-controller-other` labels.
- The old `phpunit-non-isolated-rest-controller` shard is replaced by those
  two shards.
- The REST non-controller and remaining non-isolated filters preserve their
  existing coverage boundaries.
- The production audit fails if either new shard, regex, or case arm
  disappears.
- The new shards keep production build guards and keepalive settings.
- Focused PCRE samples show representative class names match exactly one
  non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `5385dc28` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- a focused PHP PCRE partition smoke reading regex values directly from
  `.github/workflows/ci.yml`; it checked real pinned WordPress test method
  names and reported:
  - `rest-content-controller`: `970` test names, `13` classes;
  - `rest-controller-other`: `898` test names, `32` classes;
  - `rest-other`: `447` test names, `9` classes;
  - `query-theme`: `885` test names, `52` classes;
  - `block-token`: `580` test names, `68` classes;
  - `content-media`: `2613` test names, `226` classes;
  - `user-auth`: `641` test names, `41` classes;
  - `remaining`: `5597` test names, `665` classes.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI run `27896697422` on `59df9940` provided the first full production timing
for the split:

- `phpunit-non-isolated-rest-content-controller`: `71.608s` shell real,
  `68.429s` reported, `72s` total;
- `phpunit-non-isolated-rest-controller-other`: `58.761s` shell real,
  `55.723s` reported, `58s` total;
- `phpunit-non-isolated-remaining`: `121.495s` shell real, `118.374s`
  reported, `122s` total;
- `phpunit-non-isolated-content-media`: `118.110s` shell real,
  `115.029s` reported, `119s` total.

The REST controller split moved the test-only tail from REST controllers to
the broad remaining and content/media shards, so the next production timing
slice fans out both measured tails.

## Risks

- Test method count is only a proxy for runtime. The new buckets are balanced
  enough to expose whether content-object REST controllers or the remaining
  controller families drive the next production tail.
- Adding one matrix job increases artifact download/extract and Docker image
  work, but the current production run shows the slowest shard, not summed
  shard work, controls the workflow critical path.
