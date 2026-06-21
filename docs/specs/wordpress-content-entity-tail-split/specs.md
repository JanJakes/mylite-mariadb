# WordPress Content Entity Tail Split

## Problem

The query tail split moved the WordPress PHPUnit critical path away from query
classes and back to the broad content/entity shard. Green run `27911929025` on
`b8137b94` reported:

- setup total: `76s`;
- critical shard: `non-isolated-content-entity` at `99s`;
- content/entity artifact download: `7s`;
- content/entity artifact extract: `2s`;
- content/entity Docker image setup: `28s`;
- content/entity PHPUnit total: `62s`;
- content/entity PHPUnit shell real: `60.910s`;
- estimated WordPress workflow critical path: `175s`.

The same run showed that the previous query shard split worked: query-core was
`57s` total with `22.507s` shell real, and query-filter was `58s` total with
`22.904s` shell real. The next bounded wall-clock target is therefore the
content/entity shard.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_ENTITY_RE` as the visible
  content/entity shard union and
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_DATA_RE` as the broader
  remaining-shard exclusion authority.
- The current content/entity regex is
  `Tests_Term|Tests_Taxonomy|Tests_Post|Tests_Template`.
- A local static test-method proxy over the pinned WordPress sources found the
  current content/entity bucket contains `1185` test methods across `98`
  classes:
  - post/template classes: `624` test methods, `63` classes, selecting
    `Tests_Post|Tests_Template`;
  - term/taxonomy classes: `561` test methods, `35` classes, selecting
    `Tests_Term|Tests_Taxonomy`.

## Design

Replace `non-isolated-content-entity` with two visible shards:

- `non-isolated-content-post-template` /
  `phpunit-non-isolated-content-post-template`, selecting
  `Tests_Post|Tests_Template`;
- `non-isolated-content-term-taxonomy` /
  `phpunit-non-isolated-content-term-taxonomy`, selecting
  `Tests_Term|Tests_Taxonomy`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_ENTITY_RE` as the
documented union of those two sub-regexes and keep
`MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_DATA_RE` as the
remaining-shard exclusion authority. The split changes only visible matrix jobs
and test-only filter case arms.

Both shards keep the same production test-only settings as the old combined
content/entity shard: Release MyLite PHP artifacts, MinSizeRel MariaDB
embedded archive, runtime manifest verification, restored prepared baseline
database, keepalive enabled, parent install skip enabled, child install skip
disabled, default PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting individual WordPress classes by method name.
- Changing the broader content-data or remaining-shard exclusion boundaries.
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
WordPress critical path and clearer content/entity timing attribution, at the
cost of one additional parallel runner job for artifact download/extract and
Docker image reuse.

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
- Let CI provide the first full production timing for the new
  content-post-template and content-term-taxonomy shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-content-post-template` and
  `phpunit-non-isolated-content-term-taxonomy` labels.
- The old visible `phpunit-non-isolated-content-entity` matrix label and case
  arm are rejected by the production-build audit.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_ENTITY_RE` remains
  present and equals the union of the post/template and term/taxonomy
  sub-regexes in the partition proof.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_DATA_RE` remains
  the remaining-shard exclusion authority.
- Focused PCRE samples show pinned WordPress test-method proxy names match
  exactly one non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `b8137b94` plus this slice's edits.

Passed:

- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress static
  test-method proxy and reported:
  - `rest-content-post`: `609` test methods, `6` classes;
  - `rest-content-support`: `361` test methods, `7` classes;
  - `rest-controller-other`: `530` test methods, `32` classes;
  - `rest-other`: `447` test methods, `9` classes;
  - `query-filter`: `291` test methods, `7` classes;
  - `query-core`: `244` test methods, `17` classes;
  - `canonical`: `28` test methods, `11` classes;
  - `theme`: `301` test methods, `17` classes;
  - `block-token`: `519` test methods, `68` classes;
  - `media-comment`: `779` test methods, `94` classes;
  - `content-post-template`: `624` test methods, `63` classes;
  - `content-term-taxonomy`: `561` test methods, `35` classes;
  - `content-settings-meta`: `401` test methods, `34` classes;
  - `user-auth`: `637` test methods, `41` classes;
  - `remaining-platform`: `1474` test methods, `161` classes;
  - `remaining-admin-site`: `1579` test methods, `217` classes;
  - `remaining-other`: `3023` test methods, `360` classes;
  - `overlap_count=0`;
  - `content_entity_union_missing=0 extra=0 combined=1185 split=1185`.
- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for
`phpunit-non-isolated-content-post-template` and
`phpunit-non-isolated-content-term-taxonomy`.

## Risks

- Static test-method counts do not expand data-provider cases. CI timing
  remains the authority for the actual wall-clock split.
- Adding one matrix job increases total runner work. The current timing data
  shows content/entity is the wall-clock critical shard, so this is acceptable
  for a visibility and wall-clock slice.
