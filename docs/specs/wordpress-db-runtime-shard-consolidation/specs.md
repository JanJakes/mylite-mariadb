# WordPress DB And Runtime Shard Consolidation

## Problem

The first WordPress short-shard consolidation passed in CI as run
`27921136015`, but fixed per-shard setup still dominates several short jobs:

- `phpunit-db`: `7.244s` PHPUnit shell, `21s` setup, `28.244s` total;
- `phpunit-db-profile`: `7.606s` PHPUnit shell, `40s` setup, `47.606s`
  total;
- `phpunit-non-isolated-remaining-runtime-io`: `7.740s` PHPUnit shell, `23s`
  setup, `30.740s` total.

The same run reported aggregate fixed costs of `684s` Docker image work,
`137s` artifact download, and `97s` artifact extract. The next bounded
optimization is to remove short jobs whose behavior can be preserved by
separate harness invocations inside already-compatible grouped jobs.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- The WordPress `phpunit` harness restores the prepared database baseline at
  the start of each `tools/wordpress-phpunit-mysqli-mylite` invocation when
  `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`.
- The `db` and `db-profile` shards run the same `^Tests_DB` filter. The
  difference is only `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` for the
  profile pass.
- `non-isolated-remaining-runtime-io` uses the same non-isolated settings as
  `non-isolated-remaining-short`: reconnect after child enabled, child timing
  disabled, child script timing disabled, mysqli profiling disabled, keepalive
  enabled, child install skip disabled, and baseline restore disabled.

## Design

Replace the standalone `db` and `db-profile` matrix entries with one
`db-short` matrix entry. The grouped job runs two separate harness invocations:

- `phpunit-db` with the existing `^Tests_DB` filter;
- `phpunit-db-profile` with the same filter and
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`.

The grouped job records a synthetic `group-phpunit-db-short` row so shard
critical-path reporting uses the actual grouped job duration while aggregate
PHPUnit reporting still sees the original `phpunit-db` and
`phpunit-db-profile` labels.

Fold `phpunit-non-isolated-remaining-runtime-io` into the existing
`non-isolated-remaining-short` grouped job as a fourth separate harness
invocation. This removes another matrix job without changing the runtime-IO
filter or its timing label.

## Non-Goals

- Grouping deferred reconnect shards. Those shards use process-child settings
  where standalone job-level baselines are still the safer evidence boundary.
- Changing WordPress test filters, WordPress source, or PHPUnit behavior.
- Changing MyLite runtime behavior, SQL behavior, native storage, or public
  APIs.
- Claiming an engine-performance improvement.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout,
ownerless-concurrency, or WordPress application behavior changes. This is CI
scheduling and timing attribution only.

## Build And Performance Impact

The matrix loses two more shard jobs. Based on run `27921136015`, this avoids
about `61s` of per-job setup work before accounting for GitHub runner variance,
while preserving the same child filter labels and db-profile mysqli diagnostic
metrics.

The grouped `remaining-short` job is expected to stay near the current
`64s` shard critical path: previous setup was `30s`, group shell was `27s`,
and runtime-IO shell was `7.740s`.

## Test Plan

- Run shell syntax checks for `tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run focused production CTest coverage for the CI production-build audit.
- Run the production format check.
- Run `git diff --check`.
- Let the next pushed CI run provide the full production timing for the new
  grouped shard layout.

## Acceptance Criteria

- The workflow has `db-short` and no standalone `db` or `db-profile` matrix
  entries.
- The grouped DB job still emits `phpunit-db` and `phpunit-db-profile` timing
  rows, and the profile child runs with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`.
- The workflow has no standalone `non-isolated-remaining-runtime-io` matrix
  entry.
- The existing `non-isolated-remaining-short` group emits
  `phpunit-non-isolated-remaining-runtime-io` as a child timing row.
- The production-build audit requires the grouped DB/runtime layout.

## Verification Results

Local verification completed:

```text
bash -n tools/check-ci-production-builds                              # passed
tools/check-ci-production-builds                                      # passed
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
                                                                      # passed, 1/1 in 3.05s
cmake --build --preset format-check-prod                              # passed
git diff --check                                                      # passed
```

## Risks And Follow-Up

Grouped jobs run child filters sequentially on the same GitHub runner. Each
child remains a separate harness invocation and restores the prepared database
baseline at invocation start, but this still deserves full CI timing evidence.
Deferred reconnect grouping remains a separate follow-up because those shards
need a stronger per-child baseline policy before their job-level isolation is
removed.
