# WordPress REST Primary Tail Fanout

## Problem

Green CI run `27915041462` on `9f7305938` kept the WordPress PHPUnit job
green, but the timing rollup moved the current WordPress critical path to REST
test tails:

- setup action time: `72s`;
- critical shard: `non-isolated-rest-content-primary` at `67s`;
- critical shard PHPUnit shell real: `42.434s`;
- critical shard Docker image setup: `18s`;
- critical shard artifact download/extract: `3s`/`3s`;
- estimated WordPress workflow critical path: `139s`;
- next total shard: `non-isolated-rest-controller-tests-rest` at `66s`, with
  `31.098s` PHPUnit shell real and `23s` Docker image setup.

The fixed per-shard Docker and artifact costs remain the larger structural
CI-performance problem, but changing image distribution would alter the timing
environment and is larger than this bounded slice. This slice keeps the
production runtime shape constant and splits the two current REST timing tails
so the critical path can move to the next non-REST shards.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- Pinned WordPress direct `test*` method counts under
  `build/wordpress-develop/tests/phpunit/tests` show:
  - `WP_Test_REST_Posts_Controller`: `240` direct test methods;
  - `WP_Test_REST_Pages_Controller`,
    `WP_Test_REST_Attachments_Controller`, and
    `WP_Test_REST_Comments_Controller`: `292` direct test methods across
    `3` classes;
  - REST `Tests_REST*Controller` font/icon classes:
    `120` direct test methods across `4` classes;
  - remaining REST `Tests_REST*Controller` classes:
    `294` direct test methods across `13` classes.
- The current workflow uses `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REST_*`
  regex variables as the shard and exclusion authorities, and the
  production-build audit guards visible labels, shard case arms, runtime
  manifest checks, Release MyLite PHP artifacts, MinSizeRel MariaDB embedded
  artifacts, and test-only PHPUnit timing phases.

## Design

Replace two visible shards with four narrower shards:

- `non-isolated-rest-content-primary-posts` /
  `phpunit-non-isolated-rest-content-primary-posts`, selecting
  `WP_Test_REST_Posts_Controller`;
- `non-isolated-rest-content-primary-other` /
  `phpunit-non-isolated-rest-content-primary-other`, selecting
  `WP_Test_REST_Pages_Controller`,
  `WP_Test_REST_Attachments_Controller`, and
  `WP_Test_REST_Comments_Controller`;
- `non-isolated-rest-controller-tests-rest-font-icon` /
  `phpunit-non-isolated-rest-controller-tests-rest-font-icon`, selecting
  `Tests_REST_WpRestFont*Controller` and
  `Tests_REST_WpRestIconsController`;
- `non-isolated-rest-controller-tests-rest-other` /
  `phpunit-non-isolated-rest-controller-tests-rest-other`, selecting the
  remaining `Tests_REST*Controller` classes outside the content-controller
  family and outside the font/icon group.

Keep the broad REST content-controller, REST content-primary, and
`Tests_REST` controller regexes as the documented union/exclusion authorities.
The replacement positive filters are class-family filters combined with the
same non-isolated exclusion guard. They change CI timing attribution only; they
do not change the WordPress source, MyLite PHP adapter, SQL behavior, MyLite
storage, or ownerless engine code.

All replacement shards keep the same production test-only settings as the old
shards: Release MyLite PHP artifacts, MinSizeRel MariaDB embedded archive,
runtime manifest verification, restored prepared baseline database, keepalive
enabled, parent install skip enabled, child install skip disabled, default
PHPUnit logging disabled, mysqli profiling disabled, and the external transient
database directory guard.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  mysqli adapter behavior.
- Changing Docker image setup, artifact transfer, or cross-job image reuse.
- Splitting individual WordPress methods or data-provider cases.
- Claiming a final PHPUnit wall-clock floor; CI remains the timing authority.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, storage, directory, native recovery, or
WordPress application behavior changes. CI still runs the same WordPress test
surface with the same production MyLite and MariaDB artifacts.

## Directory And Lifecycle Impact

No durable layout changes. Each shard continues to restore the prepared
WordPress MyLite database baseline into an external temporary parent directory
before running the test-only phase.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, or ownerless concurrency
behavior changes.

## Build, Size, License, And Dependency Impact

No compiled-code, binary-size, license, or dependency changes. CI gains two net
WordPress PHPUnit shard jobs after replacing two REST shards with four narrower
REST shards. The expected benefit is a shorter WordPress critical path and
clearer REST timing attribution, at the cost of more parallel artifact
download/extract and Docker image reuse paths.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition proof over the pinned WordPress direct
  `test*` method proxy for the affected REST shard set.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the new REST fanout
  shards.

## Acceptance Criteria

- CI contains distinct REST primary posts, REST primary other,
  `Tests_REST` font/icon, and `Tests_REST` other labels.
- The old visible `phpunit-non-isolated-rest-content-primary` and
  `phpunit-non-isolated-rest-controller-tests-rest` matrix labels and case arms
  are rejected by the production-build audit.
- Focused PCRE samples show the old REST primary and `Tests_REST` controller
  proxy buckets are exactly covered by their replacement shard sets with no
  overlap, missing methods, or extra methods.
- The production-build audit continues to require production MyLite/MariaDB
  artifacts and separated test-only WordPress timing phases.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `9f7305938` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked pinned WordPress direct `test*`
  method proxy names for the affected REST buckets and reported:
  - `old_primary=532 split=532 posts=240 other=292 overlap=0 missing=0 extra=0`;
  - `old_tests_rest=414 split=414 font_icon=120 other=294 overlap=0 missing=0 extra=0`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI run `27915469258` passed on `a30256b3a` with `43/43` jobs green. The
published timing rollup reported setup action time at `76s`, critical shard
`non-isolated-content-term-taxonomy` at `70s`, and estimated WordPress
critical path at `146s`. The REST fanout did move the split REST tails below
the critical path:

- `non-isolated-rest-content-primary-posts`: `58s` total, including
  `21.746s` shell real, `25s` Docker image setup, `7s` artifact download, and
  `3s` artifact extract;
- `non-isolated-rest-content-primary-other`: `44s` total, including
  `22.046s` shell real, `17s` Docker image setup, `2s` artifact download, and
  `3s` artifact extract;
- `non-isolated-rest-controller-tests-rest-font-icon`: `34s` total, including
  `8.002s` shell real, `20s` Docker image setup, `2s` artifact download, and
  `3s` artifact extract;
- `non-isolated-rest-controller-tests-rest-other`: `41s` total, including
  `16.972s` shell real, `18s` Docker image setup, `4s` artifact download, and
  `2s` artifact extract.

The estimated critical path worsened from `139s` to `146s` because setup and
fixed per-shard Docker/artifact overhead drifted upward; the split removed the
REST test-body tails as the measured bottleneck but did not solve the fixed
overhead problem.

## Risks

- Static direct `test*` method counts do not expand data-provider cases. CI
  timing remains the authority for the actual wall-clock split.
- Adding two net matrix jobs increases total runner work. The current timing
  data shows REST tails as the wall-clock critical path, so this is acceptable
  for a bounded visibility and wall-clock slice.
- Fixed artifact and Docker-image overhead remains visible. Removing it would
  require a larger CI architecture change that could alter the timing
  environment.
