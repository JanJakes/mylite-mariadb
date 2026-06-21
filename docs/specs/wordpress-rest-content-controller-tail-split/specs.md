# WordPress REST Content Controller Tail Split

## Problem

The query/canonical split moved the WordPress PHPUnit tail back to the REST
content-controller shard. Green run `27911082968` on `f4864769` reported:

- setup total: `61s`;
- critical shard: `non-isolated-rest-content-controller` at `113s`;
- REST content-controller artifact download: `7s`;
- REST content-controller artifact extract: `2s`;
- REST content-controller Docker image setup: `35s`;
- REST content-controller PHPUnit total: `69s`;
- REST content-controller PHPUnit shell real: `68.785s`;
- estimated WordPress workflow critical path: `174s`.

The previous REST controller split proved that content-object REST controllers
are materially heavier than the other REST controller bucket. They now need
their own visible split before claiming the PHPUnit tail is caused by engine
startup or native open/close cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` uses
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REST_CONTENT_CONTROLLER_RE` as the
  union of the REST content-controller families.
- A local static test-method proxy over the pinned WordPress sources found the
  current REST content-controller bucket contains `970` test methods across
  `13` classes:
  - post/autosave/revision-style controllers: `609` test methods, `6`
    classes, selecting
    `Posts|Pages|Attachments|Comments|Revisions|Autosaves`;
  - support controllers: `361` test methods, `7` classes, selecting
    `Users|Tags|Categories|Taxonomies|Search|Post_Types|Post_Statuses`.

## Design

Replace `non-isolated-rest-content-controller` with two visible shards:

- `non-isolated-rest-content-post` /
  `phpunit-non-isolated-rest-content-post`, selecting
  `WP_Test_REST_(Posts|Pages|Attachments|Comments|Revisions|Autosaves)_Controller`;
- `non-isolated-rest-content-support` /
  `phpunit-non-isolated-rest-content-support`, selecting
  `WP_Test_REST_(Users|Tags|Categories|Taxonomies|Search|Post_Types|Post_Statuses)_Controller`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REST_CONTENT_CONTROLLER_RE` as the
documented union and as the negative lookahead authority for
`non-isolated-rest-controller-other`. The split changes only visible matrix
jobs and test-only filter case arms.

Both shards keep the same production test-only settings as the old combined
shard: Release MyLite PHP artifacts, MinSizeRel MariaDB embedded archive,
runtime manifest verification, restored prepared baseline database, keepalive
enabled, parent install skip enabled, child install skip disabled, default
PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting individual WordPress classes by method name.
- Changing the REST controller-other, REST non-controller, or remaining-shard
  exclusion boundaries.
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
WordPress critical path and clearer REST content-controller timing
attribution, at the cost of one additional parallel runner job for artifact
download/extract and Docker image reuse.

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
- Let CI provide the first full production timing for the new REST
  content-post and content-support shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-rest-content-post` and
  `phpunit-non-isolated-rest-content-support` labels.
- The old visible `phpunit-non-isolated-rest-content-controller` matrix label
  and case arm are rejected by the production-build audit.
- The broad
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_REST_CONTENT_CONTROLLER_RE` remains
  present and equals the union of the post and support sub-regexes in the
  partition proof.
- The REST controller-other negative lookahead still uses the broad union
  regex, so it does not capture REST content-controller tests.
- Focused PCRE samples show pinned WordPress test-method proxy names match
  exactly one non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `f4864769` plus this slice's edits.

Passed:

- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress static
  test-method proxy and reported:
  - `rest-content-post`: `609` test methods, `6` classes;
  - `rest-content-support`: `361` test methods, `7` classes;
  - `rest-controller-other`: `897` test methods, `31` classes;
  - `rest-other`: `447` test methods, `9` classes;
  - `query`: `540` test methods, `23` classes;
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
  - `rest_content_union_missing=0 extra=0 combined=970 split=970`.
- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for
`phpunit-non-isolated-rest-content-post` and
`phpunit-non-isolated-rest-content-support`.

## Risks

- Static test-method counts do not expand data-provider cases. CI timing
  remains the authority for the actual wall-clock split.
- Adding one matrix job increases total runner work. The current timing data
  shows REST content-controller is the clear wall-clock tail, so this is
  acceptable for a visibility and wall-clock slice.
