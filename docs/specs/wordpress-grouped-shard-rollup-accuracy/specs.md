# WordPress Grouped Shard Rollup Accuracy

## Problem

The first grouped WordPress PHPUnit shard run succeeded in CI, but its timing
rollup over-counted shard phases. Run `27921136015` for `f41718554` reported
`wordpress_shard_phase_count=38` even though the workflow matrix had 33 shard
jobs after consolidating five short filters into two grouped jobs.

The mismatch happened because grouped child filters still emit their original
`phpunit-*` timing labels. Those rows are correct for aggregate PHPUnit timing,
but they are not separate CI matrix jobs and should not be counted in shard
critical-path math.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `.github/workflows/ci.yml` now emits one synthetic
  `group-phpunit-<shard>` row for each grouped matrix job, while each grouped
  child filter keeps its original `phpunit-*` label.
- `tools/wordpress-phpunit-timing-rollup` already excludes
  `group-phpunit-*` rows from aggregate PHPUnit totals, but still added every
  `phpunit-*` row to the shard phase map.
- Grouped child labels have no matching `artifact-download-*`,
  `artifact-extract-*`, or `docker-image-*` setup rows. Real matrix shard jobs
  do.

## Design

Keep aggregate PHPUnit reporting unchanged: every `phpunit-*` child label still
contributes to PHPUnit totals, max PHPUnit phase reporting, and non-isolated
bucket totals.

For shard critical-path reporting, count and compare only labels that have CI
matrix setup evidence:

- `artifact-download-<shard>`;
- `artifact-extract-<shard>`;
- `docker-image-<shard>`.

Grouped matrix jobs still participate because their setup rows use the grouped
matrix shard name and their synthetic `group-phpunit-<shard>` row supplies the
group-level PHPUnit duration. Group child filter labels no longer create
pseudo-shards with zero setup cost.

## Non-Goals

- Changing WordPress test filters or workflow scheduling.
- Changing MyLite runtime behavior, SQL behavior, native storage, or public
  APIs.
- Removing per-filter grouped child timing rows.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout, or
ownerless-concurrency behavior changes. This is timing-report attribution only.

## Build And Performance Impact

No build output changes. The rollup now reports matrix shard count and
critical-path candidates from real CI shard jobs only, so follow-up performance
slices are based on accurate fixed-cost accounting.

## Test Plan

- Run shell syntax checks for the timing rollup and its fixture test.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Re-run the rollup against the downloaded timing artifact from run
  `27921136015` and confirm `wordpress_shard_phase_count=33`.
- Run focused production CTest coverage for the timing rollup.
- Run the production format check.
- Run `git diff --check`.

## Acceptance Criteria

- Grouped child `phpunit-*` rows remain in aggregate PHPUnit totals.
- Grouped child `phpunit-*` rows do not increase
  `wordpress_shard_phase_count`.
- Grouped matrix jobs still participate in critical-path reporting through
  setup rows plus `group-phpunit-*` rows.
- The existing grouped fixture proves a grouped sample has three matrix shards,
  not five pseudo-shards.

## Verification Results

Local verification completed:

```text
bash -n tools/wordpress-phpunit-timing-rollup                         # passed
bash -n tools/wordpress-phpunit-timing-rollup-test                    # passed
tools/wordpress-phpunit-timing-rollup-test                            # passed
tools/wordpress-phpunit-timing-rollup --input /tmp/mylite-ci-27921136015/timing-summary.md
  | grep -F '| timing-rollup | wordpress_shard_phase_count | 33 |'   # passed
ctest --preset prod -R '^tools\.wordpress-phpunit-timing-rollup$' --output-on-failure
                                                                      # passed, 1/1 in 0.56s
cmake --build --preset format-check-prod                              # passed
git diff --check                                                      # passed
```

## Risks And Follow-Up

If a future workflow records a PHPUnit-only matrix shard without download,
extract, or Docker setup rows, this rollup would exclude that shard from
critical-path reporting. The current production workflow records all three
setup phases before each matrix shard's PHPUnit phase, and the production-build
audit guards those timing steps.
