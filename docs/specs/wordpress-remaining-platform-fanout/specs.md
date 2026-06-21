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
  the class-prefix remaining-platform bucket contains `1567` test methods
  across `160` classes:
  - HTML/interactivity: `408` methods, `30` classes, selecting
    `Tests_HtmlApi|Tests_Interactivity_API|Tests_WP_Interactivity_API`;
  - image: `144` methods, `10` classes, selecting
    `Tests_Image|WP_Tests_Image`;
  - format/dependencies/embed: `1015` methods, `120` classes, selecting
    `Tests_Formatting|Tests_Date|Tests_Dependencies|Tests_Script_Modules|Tests_Widgets|Test_Autoembed|Tests_oEmbed|Test_oEmbed|Tests_Embed|Tests_Kses|Tests_Shortcode|REST_Block_Type_Controller_Test`.
- The corrected split matched the class-prefix remaining-platform proxy
  exactly: `overlap=0`, `missing=0`, `extra=0`.

## Design

Replace `non-isolated-remaining-platform` with three visible shards:

- `non-isolated-remaining-platform-html-interactivity` /
  `phpunit-non-isolated-remaining-platform-html-interactivity`;
- `non-isolated-remaining-platform-image` /
  `phpunit-non-isolated-remaining-platform-image`;
- `non-isolated-remaining-platform-format-dependencies-embed` /
  `phpunit-non-isolated-remaining-platform-format-dependencies-embed`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REMAINING_PLATFORM_RE` as the
documented union and as the final remaining-other exclusion authority. The
split changes only visible matrix jobs, shard case arms, and the
production-build audit. The replacement shard positives match only class-name
prefix boundaries with `^(?=(?:...)(?:_|::|$))`; PHPUnit `--filter` also
matches method names, so an unrestricted token search can pull unrelated
classes such as `Tests_Embed_Template::test_oembed_output_post` into the wrong
bucket. `Tests_Embed_*` and `WP_Tests_Image_*` are explicit class families in
the union instead of accidental substring matches, and format/dependencies/embed
stay in one shard because the first CI split showed oEmbed tests depend on
WordPress script and emoji-loader side effects from those families.

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

- CI contains distinct remaining-platform labels for HTML/interactivity, image,
  and merged format/dependencies/embed.
- The old visible `phpunit-non-isolated-remaining-platform` matrix label and
  case arm are rejected by the production-build audit.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REMAINING_PLATFORM_RE`
  remains present and stays the final remaining-other exclusion authority.
- Focused PCRE samples show pinned WordPress direct `test*` proxy names match
  exactly one non-isolated shard by class prefix or none when intentionally
  excluded.
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
  `.github/workflows/ci.yml`; it checked the corrected class-prefix
  remaining-platform proxy over pinned WordPress direct `test*` names and
  reported:
  - `platform_class_union`: `1567` test methods;
  - `remaining-platform-html-interactivity`: `408` test methods,
    `30` classes;
  - `remaining-platform-image`: `144` test methods, `10` classes;
  - `remaining-platform-format-dependencies-embed`: `1015` test methods,
    `120` classes;
  - `overlap_count=0`;
  - `remaining_platform_union_overlap=0 missing=0 extra=0 old=1567 split=1567`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

First CI run `27914070086` validated the new production job shape but failed
`phpunit-non-isolated-remaining-platform-embed-kses-shortcode`. The failing
filter used an unrestricted positive lookahead, and PHPUnit matched method
names in addition to class names; that pulled `Tests_Embed_Template` into the
embed/KSES/shortcode shard and produced WordPress harness errors unrelated to
MyLite engine behavior. The follow-up correction constrains the three
remaining-platform split positives to class-prefix boundaries, makes
`Tests_Embed_*` and `WP_Tests_Image_*` explicit, keeps
format/dependencies/embed in one side-effect-compatible shard, and updates the
production-build audit to require that shape.

CI must provide the first full production timing for the corrected
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
