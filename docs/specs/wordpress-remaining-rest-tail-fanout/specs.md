# WordPress Remaining And REST Tail Fanout

## Problem

The content/entity split moved the WordPress PHPUnit critical path to the broad
remaining-other shard, while REST controller-other became the largest pure
PHPUnit body. Green run `27912330939` on `ba2d187d` reported:

- setup total: `61s`;
- critical shard: `non-isolated-remaining-other` at `97s`;
- remaining-other artifact download: `6s`;
- remaining-other artifact extract: `3s`;
- remaining-other Docker image setup: `26s`;
- remaining-other PHPUnit total: `62s`;
- remaining-other PHPUnit shell real: `61.927s`;
- `non-isolated-rest-controller-other` at `93s` total, with `69.468s`
  PHPUnit shell real and `70s` PHPUnit total;
- estimated WordPress workflow critical path: `158s`.

Splitting only remaining-other would likely move the critical path to REST
controller-other. This slice fans out both measured tails while keeping the
same production build and test-only harness boundaries.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REST_RE` as the broad REST union and
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REST_CONTENT_CONTROLLER_RE` to keep
  content-object REST controllers outside the controller-other bucket.
- `.github/workflows/ci.yml` uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REMAINING_PLATFORM_RE` and
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REMAINING_ADMIN_SITE_RE` as explicit
  remaining-shard families. The final remaining-other bucket is the negative
  remainder after REST, query/theme, block/token, media/comment, content/data,
  user/auth, platform, and admin/site families are excluded.
- A local tokenized test-method proxy over the pinned WordPress sources found
  the old REST controller-other bucket contains `829` test methods across `32`
  classes:
  - `Tests_REST`: `344` methods, `17` classes;
  - `WP_Test_REST`: `306` methods, `8` classes;
  - `WP_REST`: `179` methods, `7` classes.
- The same proxy found the old remaining-other bucket contains `1600` test
  methods. Candidate subfamilies split that bucket into:
  - AI/connectors: `388` methods, `21` classes, selecting
    `Tests_AI|Tests_Abilities|Test_Abilities|Tests_Connectors`;
  - navigation/content-adjacent helpers: `436` methods, `80` classes,
    selecting
    `Test_WP_Customize|Tests_Link|Tests_Rewrite|Tests_Category|Tests_Sitemaps|Tests_Menu|Tests_Nav|WP_Navigation|Tests_Editor|WP_Classic`;
  - runtime/I/O helpers: `197` methods, `64` classes, selecting
    `Tests_Filesystem|WP_Filesystem|Tests_HTTP|WP_HTTP|Tests_HTTPS|Tests_Compat|Test_Compat|Tests_File|Tests_Error|Tests_Upload|Tests_Import`;
  - reduced catch-all remaining-other: `579` methods, `124` classes.

## Design

Replace `non-isolated-rest-controller-other` with three visible shards:

- `non-isolated-rest-controller-tests-rest` /
  `phpunit-non-isolated-rest-controller-tests-rest`, selecting `Tests_REST`
  controller classes that are not content-object REST controllers;
- `non-isolated-rest-controller-wp-test` /
  `phpunit-non-isolated-rest-controller-wp-test`, selecting `WP_Test_REST`
  controller classes that are not content-object REST controllers;
- `non-isolated-rest-controller-wp-rest` /
  `phpunit-non-isolated-rest-controller-wp-rest`, selecting `WP_REST`
  controller classes that are not content-object REST controllers.

Split the final remaining-other bucket into four visible shards:

- `non-isolated-remaining-ai-connectors` /
  `phpunit-non-isolated-remaining-ai-connectors`;
- `non-isolated-remaining-navigation` /
  `phpunit-non-isolated-remaining-navigation`;
- `non-isolated-remaining-runtime-io` /
  `phpunit-non-isolated-remaining-runtime-io`;
- the existing `non-isolated-remaining-other` /
  `phpunit-non-isolated-remaining-other`, now reduced by excluding the three
  new remaining subfamilies.

Keep the broad REST, platform, admin/site, and final remaining negative
lookahead boundaries as the authorities for coverage. The new remaining
subfamilies also exclude the existing REST, query/theme, block/token,
media/comment, content/data, user/auth, platform, and admin/site families so
they cannot overlap older visible shards.

All new shards keep the same production test-only settings as the retired
combined shards: Release MyLite PHP artifacts, MinSizeRel MariaDB embedded
archive, runtime manifest verification, restored prepared baseline database,
keepalive enabled, parent install skip enabled, child install skip disabled,
default PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting individual WordPress classes by method name.
- Moving classes between the existing platform or admin/site explicit shards.
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

No compiled-code, binary-size, license, or dependency changes. CI gains five
additional WordPress PHPUnit shard jobs: two from REST controller fanout and
three from remaining-other fanout. The expected benefit is a shorter WordPress
critical path and clearer timing attribution, at the cost of more parallel
artifact download/extract and Docker image reuse paths.

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
- Let CI provide the first full production timing for the new REST controller
  and remaining-other fanout shards.

## Acceptance Criteria

- CI contains distinct REST controller fanout labels for
  `phpunit-non-isolated-rest-controller-tests-rest`,
  `phpunit-non-isolated-rest-controller-wp-test`, and
  `phpunit-non-isolated-rest-controller-wp-rest`.
- The old visible `phpunit-non-isolated-rest-controller-other` matrix label and
  case arm are rejected by the production-build audit.
- CI contains distinct remaining fanout labels for
  `phpunit-non-isolated-remaining-ai-connectors`,
  `phpunit-non-isolated-remaining-navigation`, and
  `phpunit-non-isolated-remaining-runtime-io`.
- The final `phpunit-non-isolated-remaining-other` shard excludes the three new
  remaining subfamilies.
- Focused PCRE samples show pinned WordPress test-method proxy names match
  exactly one non-isolated shard or none when intentionally excluded.
- Old REST controller-other and old remaining-other proxy buckets are exactly
  covered by their replacement shard sets.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `ba2d187d` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress tokenized
  test-method proxy and reported:
  - `rest-content-post`: `179` test methods, `6` classes;
  - `rest-content-support`: `107` test methods, `7` classes;
  - `rest-controller-tests-rest`: `344` test methods, `17` classes;
  - `rest-controller-wp-test`: `306` test methods, `8` classes;
  - `rest-controller-wp-rest`: `179` test methods, `7` classes;
  - `rest-other`: `391` test methods, `9` classes;
  - `query-filter`: `189` test methods, `7` classes;
  - `query-core`: `206` test methods, `17` classes;
  - `canonical`: `29` test methods, `11` classes;
  - `theme`: `262` test methods, `17` classes;
  - `block-token`: `509` test methods, `68` classes;
  - `media-comment`: `743` test methods, `94` classes;
  - `content-post-template`: `468` test methods, `63` classes;
  - `content-term-taxonomy`: `495` test methods, `35` classes;
  - `content-settings-meta`: `335` test methods, `34` classes;
  - `user-auth`: `505` test methods, `41` classes;
  - `remaining-platform`: `1246` test methods, `161` classes;
  - `remaining-admin-site`: `1379` test methods, `217` classes;
  - `remaining-ai-connectors`: `388` test methods, `21` classes;
  - `remaining-navigation`: `436` test methods, `80` classes;
  - `remaining-runtime-io`: `197` test methods, `64` classes;
  - `remaining-other`: `579` test methods, `124` classes;
  - `overlap_count=0`;
  - `rest_controller_union_missing=0 extra=0 old=829 split=829`;
  - `remaining_other_union_missing=0 extra=0 old=1600 split=1600`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for the new fanout shards.

## Risks

- Static method counts do not expand data-provider cases. CI timing remains the
  authority for the actual wall-clock split.
- Adding five matrix jobs increases total runner work. The current timing data
  shows both tails are wall-clock candidates, so this is acceptable for a
  visibility and wall-clock slice.
