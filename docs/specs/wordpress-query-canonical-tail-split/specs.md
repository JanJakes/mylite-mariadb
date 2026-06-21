# WordPress Query Canonical Tail Split

## Problem

After the content-data tail split and the process-cost rollup work, production
CI still shows the WordPress PHPUnit wall-clock tail in the combined
query/canonical shard. Green run `27910514312` on `552ff0d4` reported:

- setup total: `63s`;
- critical shard: `non-isolated-query-canonical` at `114s`;
- query/canonical artifact download: `7s`;
- query/canonical artifact extract: `3s`;
- query/canonical Docker image setup: `20s`;
- query/canonical PHPUnit total: `84s`;
- query/canonical PHPUnit shell real: `82.489s`;
- estimated WordPress workflow critical path: `177s`.

That makes query/canonical the highest-impact visible performance target. The
new rollup also shows the process-level engine costs are much smaller than the
critical shard body: PHP process startup averaged `25.432 ms`, explicit
process connect/close averaged `147.183 ms`, profiled explicit native open
averaged `85.333 ms`, and profiled explicit native close averaged `26.729 ms`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` used
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_CANONICAL_RE` as the visible
  query/canonical shard filter and keeps
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` as the broad
  remaining-shard exclusion group.
- A local static test-method proxy over the pinned WordPress sources found the
  current query/canonical bucket contains `423` test methods across `34`
  classes:
  - `query`: `394` test methods, `23` classes, selecting
    `Tests_Query|Test_Query`;
  - `canonical`: `29` test methods, `11` classes, selecting
    `Tests_Canonical`.

## Design

Replace `non-isolated-query-canonical` with two visible shards:

- `non-isolated-query` / `phpunit-non-isolated-query`, selecting
  `Tests_Query|Test_Query`;
- `non-isolated-canonical` / `phpunit-non-isolated-canonical`, selecting
  `Tests_Canonical`.

Keep `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_CANONICAL_RE` as the
documented union of those two sub-regexes and keep
`MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` as the remaining-shard
exclusion authority. The split changes only the visible matrix jobs and
test-only filter case arms.

Both shards keep the same production test-only settings as the old combined
shard: Release MyLite PHP artifacts, MinSizeRel MariaDB embedded archive,
runtime manifest verification, restored prepared baseline database, keepalive
enabled, parent install skip enabled, child install skip disabled, default
PHPUnit logging disabled, and mysqli profiling disabled.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Splitting query classes by method name or data-provider size.
- Changing the remaining-shard exclusion regexes.
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
WordPress critical path and clearer query-versus-canonical timing attribution,
at the cost of one additional parallel runner job for artifact
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
- Let CI provide the first full production timing for the new query and
  canonical shards.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-query` and
  `phpunit-non-isolated-canonical` labels.
- The old visible `phpunit-non-isolated-query-canonical` matrix label and case
  arm are rejected by the production-build audit.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_CANONICAL_RE` remains
  present and equals the union of the query and canonical sub-regexes in the
  partition proof.
- The broad `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_QUERY_THEME_RE` remains the
  remaining-shard exclusion authority.
- Focused PCRE samples show pinned WordPress test-method proxy names match
  exactly one non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `552ff0d4` plus this slice's edits.

Passed:

- a focused PHP PCRE partition proof reading regex values directly from
  `.github/workflows/ci.yml`; it checked the pinned WordPress static
  test-method proxy and reported:
  - `rest-content-controller`: `127` test methods, `7` classes;
  - `rest-controller-other`: `737` test methods, `30` classes;
  - `rest-other`: `270` test methods, `9` classes;
  - `query`: `394` test methods, `23` classes;
  - `canonical`: `29` test methods, `11` classes;
  - `theme`: `204` test methods, `16` classes;
  - `block-token`: `474` test methods, `67` classes;
  - `media-comment`: `742` test methods, `93` classes;
  - `content-entity`: `960` test methods, `97` classes;
  - `content-settings-meta`: `335` test methods, `34` classes;
  - `user-auth`: `491` test methods, `41` classes;
  - `remaining-platform`: `1211` test methods, `157` classes;
  - `remaining-admin-site`: `1285` test methods, `209` classes;
  - `remaining-other`: `1401` test methods, `305` classes;
  - `overlap_count=0`;
  - `query_canonical_union_missing=0 extra=0 combined=423 split=423`.
- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI must provide the first full production timing for
`phpunit-non-isolated-query` and `phpunit-non-isolated-canonical`.

## Risks

- Static test-method counts do not expand data-provider cases. CI timing
  remains the authority for the actual wall-clock split.
- Adding one matrix job increases total runner work. The current timing data
  shows query/canonical is the clear wall-clock tail, so this is acceptable for
  a visibility and wall-clock slice.
