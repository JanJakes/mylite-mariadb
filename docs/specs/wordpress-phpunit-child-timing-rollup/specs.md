# WordPress PHPUnit Child Timing Rollup

## Problem

The WordPress PHPUnit timing artifact now contains process-isolated child
runtime rows and child-script rows, but the `timing-rollup` summary only
aggregates shell/PHPUnit phase totals. CI can show the raw per-shard child
metrics while still leaving the high-level question unanswered: how many child
processes ran across the process-isolated shards, how much total runtime was in
child scripts versus outer process overhead, and what are the weighted
per-child averages?

This slice makes the new child timing rows usable directly from the rollup.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` appends child timing rows to the
  WordPress timing summary when the process-isolated shards enable child timing
  and child-script timing summaries.
- `tools/wordpress-phpunit-timing-rollup` reads the markdown summary table,
  classifies rows by labels such as `phpunit-db`,
  `phpunit-deferred-reconnect-*`, `phpunit-non-isolated-*`, and
  `phpunit-db-profile`, and appends deterministic `timing-rollup` rows.
- Before this slice, the rollup accepted only the base PHPUnit shell/reported
  timing keys for `phpunit-*` rows. It skipped
  `wordpress_phpunit_child_process_*` and
  `wordpress_phpunit_child_script_*` rows, so child-script timing remained raw
  per-shard evidence.

## Design

Teach `tools/wordpress-phpunit-timing-rollup` to aggregate child metrics in the
same buckets it already emits:

- `wordpress_phpunit_test_*`,
- `wordpress_phpunit_diagnostic_*`,
- `wordpress_phpunit_all_*`,
- `wordpress_phpunit_db_*`,
- `wordpress_phpunit_isolated_*`,
- `wordpress_phpunit_non_isolated_*`,
- `wordpress_phpunit_other_test_*`.

For each bucket, sum child counters and seconds when present:

- child process count,
- closed `wpdb` count,
- static property scan counts,
- baseline-restore count and seconds,
- parent lock-release seconds,
- child-process runtime seconds,
- parent reconnect seconds,
- child-script count and seconds,
- outer-minus-script seconds.

Recompute per-child averages from summed seconds and summed counts:

- baseline restore milliseconds per restored child,
- lock-release milliseconds per child process,
- runtime milliseconds per child process,
- reconnect milliseconds per child process,
- child-script milliseconds per child script,
- outer-minus-script milliseconds per child process.

Do not sum the per-shard average rows already present in the raw summary; those
would overstate multi-shard averages. Do not change workflow filters, test
coverage, profiling mode, or production build guards.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, WordPress test
selection, or ownerless behavior changes. This is a timing-summary reporting
change only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The rollup reads and rewrites the existing
WordPress timing summary artifact when invoked with `--append`.

## Native Storage Impact

None.

## Build, Size, License, And Dependency Impact

No compiled-code, dependency, license, or binary-size impact. The shell/awk
rollup does a few more numeric additions after WordPress PHPUnit phases finish.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run the production CTest wrapper for
  `tools.wordpress-phpunit-timing-rollup`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-20:

- `bash -n tools/wordpress-phpunit-timing-rollup` passed.
- `bash -n tools/wordpress-phpunit-timing-rollup-test` passed.
- `tools/wordpress-phpunit-timing-rollup-test` passed. The fixture asserts the
  `phpunit-deferred-reconnect-baseline-restored` bucket emits child process
  counts, child script counts, runtime/script/outer-minus-script seconds, and
  recomputed `1000.000 ms`, `900.000 ms`, and `100.000 ms` child averages.
- `ctest --preset prod -R
  'tools\.wordpress-phpunit-timing-rollup|tools\.ci-production-builds'
  --output-on-failure` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed before recording these verification notes.

## Acceptance Criteria

- Rollup rows include process-isolated child count sums and child timing sums.
- Rollup rows include child-process, child-script, outer-minus-script,
  lock-release, reconnect, and baseline-restore averages recomputed from
  summed totals.
- The test fixture proves the isolated bucket emits the new rows.
- Existing build, phase, database, diagnostic, non-isolated, and perf-probe
  rollup rows remain unchanged.
- Appending the rollup remains idempotent.

## Risks And Unresolved Questions

- The rollup still summarizes timing evidence; it does not optimize PHPUnit.
- Child-script time includes PHPUnit child bootstrap, WordPress bootstrap,
  MyLite child open/close, SQL work, and test body runtime. It is useful for
  choosing the next performance target, not a pure engine microbenchmark.
