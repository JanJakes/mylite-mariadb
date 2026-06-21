# WordPress Content Data Tail Split

## Problem

After the WordPress Docker Buildx cache slice, the first hot-cache CI rerun for
`29e8cfcb` moved the WordPress PHPUnit critical path back to a real test shard:

- `non-isolated-content-data`: `117s` total, with `8s` artifact download,
  `3s` artifact extract, `36s` Docker image setup, and `70s` PHPUnit total;
- `non-isolated-rest-content-controller`: `104s` total, with `74s` PHPUnit
  total;
- `non-isolated-query-canonical`: `104s` total, with `78s` PHPUnit total.

The hot-cache estimated WordPress workflow critical path was `184s`. Splitting
only Docker setup is no longer the whole answer: the current wall-clock gate is
the broad `content-data` shard plus its fixed per-shard overhead.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` derives non-isolated shard filters from
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER` plus visible shard-family
  regexes.
- The current broad content-data regex is
  `Tests_Term|Tests_Taxonomy|Tests_Post|Tests_Template|Tests_Customize|Tests_WP_Customize|Tests_Option|Tests_Meta`.
  It is also used by remaining-shard negative lookaheads as the broad
  exclusion authority.
- A local scan of pinned WordPress test method names found the current
  content-data regex covers `1622` test names across `132` classes.
- Splitting the current regex into two subfamilies yields:
  - `content-entity`: `1200` test names, `98` classes, covering
    `Tests_Term|Tests_Taxonomy|Tests_Post|Tests_Template`;
  - `content-settings-meta`: `422` test names, `34` classes, covering
    `Tests_Customize|Tests_WP_Customize|Tests_Option|Tests_Meta`.
- The pinned tree also has singular `Test_WP_Customize_*` classes. They are
  not matched by the current content-data regex, so this slice deliberately
  does not move them.

## Design

Replace `non-isolated-content-data` with two visible matrix shards:

- `non-isolated-content-entity` /
  `phpunit-non-isolated-content-entity`, selecting
  `Tests_Term|Tests_Taxonomy|Tests_Post|Tests_Template`;
- `non-isolated-content-settings-meta` /
  `phpunit-non-isolated-content-settings-meta`, selecting
  `Tests_Customize|Tests_WP_Customize|Tests_Option|Tests_Meta`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_DATA_RE` as the broad
remaining-shard exclusion authority. Add the two sub-regexes only for the
visible shard filters, matching the existing query/theme split pattern.

Both new shards keep the previous production test-only settings: Release
MyLite PHP artifacts, MinSizeRel MariaDB embedded archive, runtime manifest
verification, restored baseline database, keepalive enabled, parent install
skip enabled, child install skip disabled, default PHPUnit logging disabled,
and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Moving the singular `Test_WP_Customize_*` classes into content-data.
- Splitting individual WordPress classes by method name.
- Claiming that CI shard fanout is an engine throughput improvement.

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
additional WordPress PHPUnit shard job. The expected benefit is lower
wall-clock critical path after setup; the cost is one more parallel artifact
download/extract and Docker image reuse path.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition proof over pinned WordPress test method
  names for the full non-isolated shard set.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the new content shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-content-entity` and
  `phpunit-non-isolated-content-settings-meta` labels.
- The old `phpunit-non-isolated-content-data` label is replaced by those two
  labels.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_CONTENT_DATA_RE` remains the
  remaining-shard exclusion authority.
- The production audit fails if either new shard, regex, or case arm
  disappears.
- Focused PCRE samples show pinned WordPress test method names match exactly
  one non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `e278b723` plus this slice's edits.

Passed:

- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked real pinned WordPress test method
  names and reported:
  - `rest-content-controller`: `970` test names, `13` classes;
  - `rest-controller-other`: `897` test names, `31` classes;
  - `rest-other`: `447` test names, `9` classes;
  - `query-canonical`: `569` test names, `34` classes;
  - `theme`: `316` test names, `17` classes;
  - `block-token`: `579` test names, `67` classes;
  - `media-comment`: `991` test names, `94` classes;
  - `content-entity`: `1200` test names, `98` classes;
  - `content-settings-meta`: `422` test names, `34` classes;
  - `user-auth`: `646` test names, `41` classes;
  - `remaining-platform`: `1789` test names, `160` classes;
  - `remaining-admin-site`: `1656` test names, `217` classes;
  - `remaining-other`: `1701` test names, `267` classes.
- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI run `27898995461` for `a2a0b8f4` passed. The production timing rollup
reported:

- setup: `71s`;
- `non-isolated-content-entity`: `87s` total, with `61.447s` PHPUnit shell;
- `non-isolated-content-settings-meta`: `50s` total, with `12.382s` PHPUnit
  shell;
- critical shard: `non-isolated-query-canonical` at `106s`, with `5s`
  artifact download, `2s` artifact extract, `35s` Docker image setup, and
  `64s` PHPUnit total;
- max PHPUnit shell body:
  `phpunit-non-isolated-rest-content-controller` at `73.871s`;
- estimated WordPress workflow critical path: `177s`;
- total shard Docker setup rows: `473s`.

Compared with the preceding hot-cache Buildx timing (`184s` estimated critical
path), the slice removed `non-isolated-content-data` as the critical shard but
only bought about `7s` wall-clock because `query-canonical` and REST content
were already close behind it.

## Risks

- Test-name and class counts are only runtime proxies. CI timing remains the
  authority for whether the critical path moves.
- Adding one matrix job increases total runner work. The hot-cache timing data
  shows the current gate is still shard-bound, so this is acceptable for a
  wall-clock and timing-visibility slice.
