# WordPress Top Tail Fanout

## Problem

The REST controller split in CI run `27896697422` on `59df9940` moved the
WordPress PHPUnit critical path away from REST controllers, but the next two
non-isolated tails are close enough that splitting only one would leave the
other as the wall-clock gate:

- `phpunit-non-isolated-remaining`: `121.495s` shell real, `118.374s`
  reported, `122s` total;
- `phpunit-non-isolated-content-media`: `118.110s` shell real, `115.029s`
  reported, `119s` total;
- `phpunit-non-isolated-query-theme`: `105.144s` shell real, `101.940s`
  reported, `105s` total;
- `phpunit-non-isolated-rest-content-controller`: `71.608s` shell real,
  `68.429s` reported, `72s` total;
- `phpunit-non-isolated-rest-controller-other`: `58.761s` shell real,
  `55.723s` reported, `58s` total.

The timing rollup identified `non-isolated-remaining` as the shard critical
path at `158s`, with estimated WordPress workflow critical path at `236s`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` derives all non-isolated filters from
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER`.
- The current broad content/media shard has `226` classes and `2613` pinned
  WordPress test names. Path-segment counts are led by `post` (`603`), `term`
  (`490`), `comment` (`445`), `xmlrpc` (`277`), and `media` (`265`).
- The current remaining shard has `665` classes and `5597` pinned WordPress
  test names. Path-segment counts are spread across `multisite` (`403`),
  `formatting` (`371`), `admin` (`320`), `html-api` (`289`), `functions`
  (`278`), `customize` (`277`), `interactivity-api` (`248`), `media` (`223`),
  `l10n` (`202`), `general` (`197`), and many smaller families.
- The edited workflow partition proof over pinned WordPress test method names
  produced:
  - `media-comment`: `991` test names, `94` classes;
  - `content-data`: `1622` test names, `132` classes;
  - `remaining-platform`: `2011` test names, `161` classes;
  - `remaining-admin-site`: `1653` test names, `217` classes;
  - `remaining-other`: `1933` test names, `287` classes.

## Design

Replace the measured top tails with deterministic class-family fanout:

- `non-isolated-media-comment` /
  `phpunit-non-isolated-media-comment`, selecting
  `Tests_Media|Tests_Attachment|Tests_Comment|Tests_XMLRPC|Tests_Xmlrpc`;
- `non-isolated-content-data` / `phpunit-non-isolated-content-data`, selecting
  `Tests_Term|Tests_Taxonomy|Tests_Post|Tests_Template|Tests_Customize|Tests_WP_Customize|Tests_Option|Tests_Meta`;
- `non-isolated-remaining-platform` /
  `phpunit-non-isolated-remaining-platform`, selecting remaining tests that
  match
  `Test_Autoembed|Tests_HtmlApi|Tests_Dependencies|Tests_Script_Modules|Tests_Interactivity_API|Tests_WP_Interactivity_API|Tests_Kses|Tests_Formatting|Tests_Shortcode|Tests_Widgets|Tests_Date|Tests_Image|Tests_oEmbed|Test_oEmbed|REST_Block_Type_Controller_Test`;
- `non-isolated-remaining-admin-site` /
  `phpunit-non-isolated-remaining-admin-site`, selecting remaining tests that
  match
  `Tests_Admin|Tests_Ajax|Tests_Multisite|Tests_General|Tests_Functions|Tests_Cron|Tests_L10n|Tests_Pluggable|WP_Translation`;
- `non-isolated-remaining-other` /
  `phpunit-non-isolated-remaining-other`, selecting remaining tests that do
  not match either remaining subfamily.

All five new shards keep the previous production test-only settings: Release
MyLite PHP artifacts, MinSizeRel MariaDB embedded archive, runtime manifest
verification, restored baseline database, keepalive enabled, parent install
skip enabled, child install skip disabled, default PHPUnit logging disabled,
and mysqli profiling disabled.

Existing DB, process-isolated, REST, query/theme, block/token, and user/auth
boundaries stay unchanged.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Enabling default mysqli profiling on broad non-isolated shards.
- Claiming that shard fanout is an engine throughput improvement.
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

No compiled-code, binary-size, license, or dependency changes. CI gains three
additional WordPress PHPUnit shard jobs. The goal is lower wall-clock critical
path and clearer timing attribution, at the cost of more parallel runner work
for artifact download/extract and Docker image reuse.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition proof over pinned WordPress test method
  names for DB, process-isolated exclusions, REST content controller, REST
  controller-other, REST non-controller, query/theme, block/token,
  media/comment, content/data, user/auth, remaining/platform,
  remaining/admin-site, and remaining/other.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the five new fanout
  shards.

## Acceptance Criteria

- CI contains distinct timing labels for the five new fanout shards.
- The old `phpunit-non-isolated-content-media` and
  `phpunit-non-isolated-remaining` labels are replaced by the new labels.
- The production audit fails if any new shard, regex, or case arm disappears.
- Existing REST, query/theme, block/token, and user/auth boundaries remain
  unchanged.
- Focused PCRE samples show pinned WordPress test method names match exactly
  one non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `59df9940` plus this slice's edits.

Passed:

- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked real pinned WordPress test method
  names and reported:
  - `rest-content-controller`: `970` test names, `13` classes;
  - `rest-controller-other`: `898` test names, `32` classes;
  - `rest-other`: `447` test names, `9` classes;
  - `query-theme`: `885` test names, `52` classes;
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

CI run `27897052195` on `eec8f351` provided the first full production timing
for the fanout:

- `phpunit-non-isolated-content-data`: `72.858s` shell real, `69.790s`
  reported, `74s` total;
- `phpunit-non-isolated-media-comment`: `44.906s` shell real, `41.710s`
  reported, `45s` total;
- `phpunit-non-isolated-remaining-admin-site`: `26.378s` shell real,
  `23.001s` reported, `27s` total;
- `phpunit-non-isolated-remaining-other`: `61.672s` shell real, `58.537s`
  reported, `62s` total;
- `phpunit-non-isolated-remaining-platform`: `37.853s` shell real,
  `34.709s` reported, `38s` total.

The fanout moved the test-only tail to `phpunit-non-isolated-query-theme` at
`105.802s` shell real, so the next timing slice splits query/canonical from
theme tests.

## Risks

- Test-name and class-family counts are only runtime proxies. CI timing remains
  the authority for whether the new wall-clock tail moved.
- Adding three matrix jobs increases total runner work. The latest timing data
  shows the critical path is still shard-bound, so this is acceptable for a
  visibility and wall-clock slice.
