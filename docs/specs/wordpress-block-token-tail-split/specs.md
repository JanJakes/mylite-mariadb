# WordPress Block Token Tail Split

## Problem

The REST and remaining fanout moved the WordPress PHPUnit critical path to the
block/token shard. Green run `27912810694` on `1f10f5a3` reported:

- setup total: `70s`;
- critical shard: `non-isolated-block-token` at `105s`;
- block/token artifact download: `23s`;
- block/token artifact extract: `3s`;
- block/token Docker image setup: `22s`;
- block/token PHPUnit total: `57s`;
- block/token PHPUnit shell real: `56.428s`;
- estimated WordPress workflow critical path: `175s`.

The prior split worked: the old REST controller-other and remaining-other tails
no longer controlled the job. This slice targets only the measured block/token
tail while preserving the production-build timing shape.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_BLOCK_TOKEN_RE` as the visible
  block/token shard union and as part of the remaining-shard exclusion
  boundary.
- A local direct `test*` method proxy over the pinned WordPress sources found
  the old block/token bucket contains `474` test methods across `67` classes:
  - `Tests_Blocks*` library classes: `287` methods, `31` classes;
  - block supports/templates/bindings/patterns, `Tests_WpTokenMap`, and small
    `WP_Block*` classes: `187` methods, `36` classes.
- The candidate split matched the old direct test-method proxy exactly:
  `overlap=0`, `missing=0`, `extra=0`.

## Design

Replace `non-isolated-block-token` with two visible shards:

- `non-isolated-block-library` /
  `phpunit-non-isolated-block-library`, selecting `Tests_Blocks*`;
- `non-isolated-block-support-template` /
  `phpunit-non-isolated-block-support-template`, selecting
  `Tests_WpTokenMap`, `WP_Block*`, and
  `Tests_Block_(Bindings|Pattern|Supports|Template|Templates)*`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_BLOCK_TOKEN_RE` as the documented
union and remaining-shard exclusion authority. The split changes only visible
matrix jobs, shard case arms, and the production-build audit.

Both shards keep the same production test-only settings as the old combined
block/token shard: Release MyLite PHP artifacts, MinSizeRel MariaDB embedded
archive, runtime manifest verification, restored prepared baseline database,
keepalive enabled, parent install skip enabled, child install skip disabled,
default PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting individual WordPress classes by method name.
- Changing the broad block/token remaining-shard exclusion boundary.
- Optimizing Docker image setup, artifact transfer, native open/close, or
  ownerless engine execution in this slice.

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
WordPress critical path and clearer block/token timing attribution, at the cost
of one additional parallel artifact download/extract and Docker image reuse
path.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition proof over the pinned WordPress direct
  `test*` method proxy for the full non-isolated shard set.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the new block/library and
  block/support-template shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-block-library` and
  `phpunit-non-isolated-block-support-template` labels.
- The old visible `phpunit-non-isolated-block-token` matrix label and case arm
  are rejected by the production-build audit.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_BLOCK_TOKEN_RE` remains
  present and stays the remaining-shard exclusion authority.
- Focused PCRE samples show pinned WordPress direct `test*` proxy names match
  exactly one non-isolated shard or none when intentionally excluded.
- The old block/token proxy bucket is exactly covered by the replacement shard
  set.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `1f10f5a3` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress direct `test*`
  method proxy and reported:
  - `rest-content-post`: `53` test methods, `3` classes;
  - `rest-content-support`: `74` test methods, `4` classes;
  - `rest-controller-tests-rest`: `315` test methods, `15` classes;
  - `rest-controller-wp-test`: `243` test methods, `8` classes;
  - `rest-controller-wp-rest`: `179` test methods, `7` classes;
  - `rest-other`: `270` test methods, `9` classes;
  - `query-filter`: `189` test methods, `7` classes;
  - `query-core`: `205` test methods, `16` classes;
  - `canonical`: `29` test methods, `11` classes;
  - `theme`: `204` test methods, `16` classes;
  - `block-library`: `287` test methods, `31` classes;
  - `block-support-template`: `187` test methods, `36` classes;
  - `media-comment`: `742` test methods, `93` classes;
  - `content-post-template`: `465` test methods, `62` classes;
  - `content-term-taxonomy`: `495` test methods, `35` classes;
  - `content-settings-meta`: `335` test methods, `34` classes;
  - `user-auth`: `491` test methods, `41` classes;
  - `remaining-platform`: `1211` test methods, `157` classes;
  - `remaining-admin-site`: `1285` test methods, `209` classes;
  - `remaining-ai-connectors`: `98` test methods, `20` classes;
  - `remaining-navigation`: `430` test methods, `80` classes;
  - `remaining-runtime-io`: `194` test methods, `61` classes;
  - `remaining-other`: `1389` test methods, `179` classes;
  - `overlap_count=0`;
  - `block_token_union_missing=0 extra=0 old=474 split=474`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for
`phpunit-non-isolated-block-library` and
`phpunit-non-isolated-block-support-template`.

## Risks

- Static direct `test*` method counts do not expand data-provider cases. CI
  timing remains the authority for the actual wall-clock split.
- Adding one matrix job increases total runner work. The current timing data
  shows block/token is the wall-clock critical shard, so this is acceptable for
  a visibility and wall-clock slice.
