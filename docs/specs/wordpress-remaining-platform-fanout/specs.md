# WordPress Remaining Platform Fanout

## Problem

The current top-tail fanout passed in CI run `27913693745` on `e125e19d`, but
the next timing sample made `non-isolated-remaining-platform` the critical
WordPress shard:

- setup total: `73s`;
- critical shard: `non-isolated-remaining-platform` at `73s`;
- remaining-platform artifact download: `6s`;
- remaining-platform artifact extract: `3s`;
- remaining-platform Docker image setup: `24s`;
- remaining-platform PHPUnit total: `40s`;
- remaining-platform PHPUnit shell real: `38.804s`;
- estimated WordPress workflow critical path: `146s`;
- slowest pure PHPUnit body:
  `phpunit-non-isolated-rest-content-primary` at `49.178s` shell real.

The previous split reduced the old block and media/comment bodies, but the
remaining-platform bucket still groups unrelated platform-helper families. This
slice splits that measured critical shard while keeping the production build
and test-only runtime shape constant.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REMAINING_PLATFORM_RE` as the visible
  remaining-platform shard union and as part of the final remaining-other
  exclusion boundary.
- A local direct `test*` method proxy over the pinned WordPress sources found
  the old remaining-platform bucket contains `1211` test methods across `157`
  classes:
  - HTML/interactivity: `305` methods, `30` classes, selecting
    `Tests_HtmlApi|Tests_Interactivity_API|Tests_WP_Interactivity_API`;
  - formatting/date: `475` methods, `85` classes, selecting
    `Tests_Formatting|Tests_Date`;
  - image: `143` methods, `10` classes, selecting `Tests_Image`;
  - dependencies/widgets: `143` methods, `20` classes, selecting
    `Tests_Dependencies|Tests_Script_Modules|Tests_Widgets`;
  - embed/KSES/shortcode: `145` methods, `12` classes, selecting
    `Test_Autoembed|Tests_oEmbed|Test_oEmbed|Tests_Kses|Tests_Shortcode|REST_Block_Type_Controller_Test`.
- The candidate split matched the old direct test-method proxy exactly:
  `overlap=0`, `missing=0`, `extra=0`.

## Design

Replace `non-isolated-remaining-platform` with five visible shards:

- `non-isolated-remaining-platform-html-interactivity` /
  `phpunit-non-isolated-remaining-platform-html-interactivity`;
- `non-isolated-remaining-platform-format-date` /
  `phpunit-non-isolated-remaining-platform-format-date`;
- `non-isolated-remaining-platform-image` /
  `phpunit-non-isolated-remaining-platform-image`;
- `non-isolated-remaining-platform-dependencies-widgets` /
  `phpunit-non-isolated-remaining-platform-dependencies-widgets`;
- `non-isolated-remaining-platform-embed-kses-shortcode` /
  `phpunit-non-isolated-remaining-platform-embed-kses-shortcode`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REMAINING_PLATFORM_RE` as the
documented union and as the final remaining-other exclusion authority. The
split changes only visible matrix jobs, shard case arms, and the
production-build audit.

All replacement shards keep the same production test-only settings as the old
remaining-platform shard: Release MyLite PHP artifacts, MinSizeRel MariaDB
embedded archive, runtime manifest verification, restored prepared baseline
database, keepalive enabled, parent install skip enabled, child install skip
disabled, default PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting individual WordPress classes by method name.
- Changing the broad remaining-platform or final remaining-other exclusion
  boundaries.
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

No compiled-code, binary-size, license, or dependency changes. CI gains four
additional WordPress PHPUnit shard jobs. The expected benefit is a shorter
WordPress critical path and clearer remaining-platform timing attribution, at
the cost of more parallel artifact download/extract and Docker image reuse
paths.

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
- Let CI provide the first full production timing for the new
  remaining-platform fanout shards.

## Acceptance Criteria

- CI contains distinct remaining-platform labels for HTML/interactivity,
  formatting/date, image, dependencies/widgets, and embed/KSES/shortcode.
- The old visible `phpunit-non-isolated-remaining-platform` matrix label and
  case arm are rejected by the production-build audit.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REMAINING_PLATFORM_RE`
  remains present and stays the final remaining-other exclusion authority.
- Focused PCRE samples show pinned WordPress direct `test*` proxy names match
  exactly one non-isolated shard or none when intentionally excluded.
- The old remaining-platform proxy bucket is exactly covered by the
  replacement shard set.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `e125e19d` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress direct `test*`
  method proxy and reported:
  - `rest-content-primary`: `34` test methods, `1` class;
  - `rest-content-history`: `19` test methods, `2` classes;
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
  - `block-supports`: `93` test methods, `24` classes;
  - `block-template-binding`: `94` test methods, `12` classes;
  - `media`: `59` test methods, `16` classes;
  - `comment`: `419` test methods, `38` classes;
  - `xmlrpc`: `264` test methods, `39` classes;
  - `content-post-template`: `465` test methods, `62` classes;
  - `content-term-taxonomy`: `495` test methods, `35` classes;
  - `content-settings-meta`: `335` test methods, `34` classes;
  - `user-auth`: `491` test methods, `41` classes;
  - `remaining-platform-html-interactivity`: `305` test methods,
    `30` classes;
  - `remaining-platform-format-date`: `475` test methods, `85` classes;
  - `remaining-platform-image`: `143` test methods, `10` classes;
  - `remaining-platform-dependencies-widgets`: `143` test methods,
    `20` classes;
  - `remaining-platform-embed-kses-shortcode`: `145` test methods,
    `12` classes;
  - `remaining-admin-site`: `1285` test methods, `209` classes;
  - `remaining-ai-connectors`: `98` test methods, `20` classes;
  - `remaining-navigation`: `430` test methods, `80` classes;
  - `remaining-runtime-io`: `194` test methods, `61` classes;
  - `remaining-other`: `1389` test methods, `179` classes;
  - `overlap_count=0`;
  - `remaining_platform_union_overlap=0 missing=0 extra=0 old=1211 split=1211`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for the new
remaining-platform fanout shards.

## Risks

- Static direct `test*` method counts do not expand data-provider cases. CI
  timing remains the authority for the actual wall-clock split.
- Adding four matrix jobs increases total runner work. The current timing data
  shows remaining-platform as the wall-clock critical shard, so this is
  acceptable for a visibility and wall-clock slice.
- Fixed artifact and Docker-image overhead remains visible. Removing it would
  require a larger CI architecture change that could alter the timing
  environment; this slice keeps the test runtime shape constant.
