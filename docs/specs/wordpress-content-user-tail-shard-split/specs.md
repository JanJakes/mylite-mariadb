# WordPress Content User Tail Shard Split

## Problem

The split WordPress PHPUnit CI job now separates build/setup from test-only
shards, but the latest green production run still has a long non-isolated
tail. CI run `27895890622` on `cf58e53a` reported:

- `phpunit-non-isolated-content-user`: `139.988s` shell real, `136.807s`
  reported, `140s` total;
- `phpunit-non-isolated-rest-controller`: `127.890s` shell real, `124.660s`
  reported, `129s` total;
- `phpunit-non-isolated-remaining`: `115.693s` shell real, `112.606s`
  reported, `116s` total;
- all build/setup, embedded, compiler, and process-isolated shards were already
  shorter than those non-isolated tails.

The broad content/user shard hides whether the remaining wall-time pressure is
content/media workload, user/auth workload, or both. It also controls the
current CI wall-time after setup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `.github/workflows/ci.yml` defines the WordPress PHPUnit matrix and derives
  each test-only shard filter from
  `MYLITE_WORDPRESS_PHPUNIT_NON_ISOLATED_FILTER`.
- The existing broad content/user regex matches content, media, taxonomy,
  comments, options, metadata, customization, XML-RPC, users, auth, roles,
  capabilities, and privacy classes.
- `tools/check-ci-production-builds` audits the workflow for production build
  type, split test-only execution, timing summaries, keepalive, and shard
  filter boundaries.

## Design

Replace the single `non-isolated-content-user` matrix entry with two entries:

- `non-isolated-content-media` /
  `phpunit-non-isolated-content-media`, selecting
  `Tests_Term|Tests_Taxonomy|Tests_Post|Tests_Comment|Tests_Media|Tests_Attachment|Tests_Template|Tests_XMLRPC|Tests_Xmlrpc|Tests_Customize|Tests_WP_Customize|Tests_Option|Tests_Meta`;
- `non-isolated-user-auth` / `phpunit-non-isolated-user-auth`, selecting
  `Tests_User|Tests_Auth|Tests_Roles|Tests_Capabilities|Tests_Privacy`.

Both shards keep the same production test-only settings as the old shard:
Release MyLite PHP artifacts, MinSizeRel MariaDB archive, keepalive enabled,
install skip enabled at the parent, child install skip disabled, mysqli
profiling disabled, and runtime manifest verification enabled.

Update the remaining non-isolated shard's negative lookahead to exclude both
new regex families. This keeps the non-isolated split as a partition of the
previous suite: DB and process-isolated exclusions remain the authority, broad
REST/query-theme/block-token families remain separate, and only the former
content/user family is divided.

## Non-Goals

- Changing SQL behavior, MyLite engine behavior, WordPress test behavior, or
  compatibility expectations.
- Enabling mysqli profiling on long non-isolated shards by default.
- Splitting REST controller classes without class-time evidence.
- Claiming the ownerless concurrency objective is complete.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, storage, directory, or WordPress application
behavior changes. CI still runs the same WordPress test surface; only the
production timing and test shard boundaries change.

## Directory And Lifecycle Impact

No durable layout changes. Each shard continues to restore the same prepared
WordPress MyLite database baseline into the same external temporary parent
directory before its test-only phase.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, or ownerless concurrency
behavior changes.

## Build, Size, License, And Dependency Impact

No compiled-code, binary-size, license, or dependency changes. CI gains one
additional WordPress PHPUnit shard invocation. The per-shard artifact
download/extract and Docker image reuse overhead was `5-8s` in the latest run,
while the old broad shard took `140s`; the expected wall-time impact is lower
max-shard time at the cost of one more parallel job.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run a focused PHP PCRE partition sample for DB, process-isolated exclusions,
  REST, query/theme, block/token, content/media, user/auth, and remaining class
  names.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `git diff --check`.
- Let CI provide the first full production timing for the two new shards,
  because the full non-isolated WordPress suite is too slow to duplicate
  locally for a workflow partitioning change.

## Acceptance Criteria

- CI contains distinct `phpunit-non-isolated-content-media` and
  `phpunit-non-isolated-user-auth` labels.
- The old `phpunit-non-isolated-content-user` label and shard are gone.
- The remaining non-isolated filter excludes both new regex families.
- The production audit fails if either new shard, regex, or case arm disappears.
- The new shards keep production build guards and keepalive settings.
- Focused PCRE samples show representative class names match exactly one
  non-isolated shard or none when intentionally excluded.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `cf58e53a` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `tools/check-ci-production-builds`
- a focused PHP PCRE partition smoke that reads the regex values directly from
  `.github/workflows/ci.yml` and verifies representative DB,
  process-isolated, REST controller, REST non-controller, query/theme,
  block/token, content/media, user/auth, and remaining class names;
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

CI run `27896266558` on `5385dc28` provided the first full production timing
for the split:

- `phpunit-non-isolated-content-media`: `117.366s` shell real,
  `114.134s` reported, `118s` total;
- `phpunit-non-isolated-user-auth`: `30.253s` shell real, `27.034s`
  reported, `31s` total;
- `phpunit-non-isolated-rest-controller`: `147.060s` shell real,
  `143.930s` reported, `147s` total;
- `phpunit-non-isolated-remaining`: `122.608s` shell real, `119.399s`
  reported, `124s` total.

The content/user split reduced the old broad content/user timing boundary and
made REST controller classes the next visible production critical path.

## Risks

- Runtime may not split evenly: content/media is likely larger than user/auth.
  This still gives the next production run sharper evidence than the old broad
  content/user bucket.
- Adding one matrix job increases artifact download/extract and Docker image
  reuse overhead. The latest timing data indicates max-shard runtime dominates
  over this overhead.
