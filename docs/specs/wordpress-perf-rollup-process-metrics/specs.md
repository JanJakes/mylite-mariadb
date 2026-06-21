# WordPress Perf Rollup Process Metrics

## Problem

The WordPress `perf-probe` timing summary carries process startup, explicit
process connect/close, implicit object-free process connect/close, and native
open/close profile rows. The final CI rollup kept the older combined
process-connect rows but dropped the newer explicit/implicit close-shape and
native profile rows. That made the most useful per-process startup/open/close
evidence visible in the setup step while hiding it from the combined timing
artifact used for branch/main comparisons.

## Source Findings

- `tools/wordpress-phpunit-mysqli-mylite` appends
  `wordpress_perf_summary_php_connect_process_*` and
  `wordpress_perf_summary_mysqli_process_*_profile_*` rows during the
  `perf-probe` phase.
- `tools/wordpress-phpunit-timing-rollup` only copied the older
  `wordpress_perf_summary_php_process_connect_close_*` aliases plus steady
  SQL/connect rates.
- `tools/wordpress-phpunit-timing-rollup-test` already covers the synthetic
  rollup table and is the right regression point for final summary rows.
- `tools/check-ci-production-builds` already guards the timing scripts and can
  fail future edits that silently drop the process attribution rows.

## Design

Extend the timing rollup's phase metric allow-list and emitted rows with:

- explicit-close process connect/close average;
- implicit object-free process connect/close average;
- explicit and implicit process-minus-startup deltas;
- implicit-minus-explicit process delta;
- process connect/close iteration count;
- explicit and implicit native open/close profile counts and timings.

Keep the source summary row names unchanged. The final rollup rows remove only
the existing `wordpress_perf_summary_` prefix, matching the rollup's current
style for other performance-probe rows.

## Compatibility Impact

No SQL behavior, PHP API, mysqli API, C API, storage, ownerless concurrency,
database directory, or native recovery behavior changes. This is CI reporting
only.

## Performance Impact

No production runtime cost. The final rollup does a few more AWK map lookups
over the already generated Markdown timing table.

## Test Plan

- Run shell syntax checks for the rollup, rollup fixture, and CI production
  audit scripts.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest wrappers for
  `tools.wordpress-phpunit-timing-rollup` and `tools.ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- The rollup fixture proves explicit and implicit process close-shape rows are
  preserved.
- The rollup fixture proves explicit and implicit native open/close profile
  rows are preserved.
- The production audit guards representative summary and final rollup metric
  names.
- Existing rollup rows and idempotent append behavior remain unchanged.

## Verification Results

Local verification on 2026-06-21 passed:

- `bash -n tools/wordpress-phpunit-timing-rollup`
- `bash -n tools/wordpress-phpunit-timing-rollup-test`
- `bash -n tools/check-ci-production-builds`
- `tools/wordpress-phpunit-timing-rollup-test`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The synthetic rollup fixture now proves the final timing rollup preserves
explicit process close-shape rows, implicit process close-shape rows, and the
explicit/implicit native open/close profile rows.
