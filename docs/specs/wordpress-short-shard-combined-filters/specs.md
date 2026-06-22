# WordPress Short Shard Combined Filters

## Problem

The WordPress PHPUnit CI job now separates production build/setup, artifact
transport, Docker materialization, and test-only execution. Green run
`27968595768` on `5037ab11c` showed that zstd artifact transport is no longer
the largest repeated fixed cost:

- `wordpress_setup_total_seconds_sum`: `65s`;
- `wordpress_shard_critical_path_label`:
  `non-isolated-block-library-supports-short`;
- `wordpress_shard_critical_path_seconds_max`: `67s`;
- `wordpress_shard_critical_path_docker_image_seconds`: `24s`;
- `wordpress_shard_critical_path_phpunit_shell_real_seconds`: `36s`;
- `wordpress_phpunit_test_shell_overhead_seconds_sum`: `110.861s`;
- `wordpress_docker_build_seconds`: `586s`;
- `wordpress_artifact_download_seconds`: `83s`;
- `wordpress_artifact_extract_seconds`: `15s`.

Several already-consolidated short shards still launched two to four separate
PHPUnit processes after the artifact and Docker image were ready. Each child
launch paid roughly the same PHPUnit/container wrapper overhead as a standalone
shard. That kept timing detail high, but it also made short grouped shards a
critical-path tail.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- The affected shards are all non-isolated WordPress shards with the same CI
  settings: reconnect after child enabled, child timing disabled, child script
  timing disabled, mysqli profiling disabled, keepalive enabled, child install
  skip disabled, and baseline restore disabled.
- `.github/workflows/ci.yml` already provides one matrix-level timing label per
  grouped shard, for example
  `phpunit-non-isolated-block-library-supports-short`.
- `tools/wordpress-phpunit-timing-rollup` can compute shard critical path from
  normal `phpunit-<shard>` labels. Synthetic `group-phpunit-*` rows are only
  required when a job intentionally emits multiple child timing labels.
- The `db-short` shard is different: it must keep two child invocations because
  the second `^Tests_DB` pass enables mysqli profiling diagnostics.

## Design

Keep the matrix shape and production build requirements unchanged, but replace
the same-mode short child invocations with one combined PHPUnit filter per
shard:

- `non-isolated-rest-short`;
- `non-isolated-rest-controller-wp-short`;
- `non-isolated-block-library-supports-short`;
- `non-isolated-content-media-short`;
- `non-isolated-remaining-short`.

The workflow adds `combine_wordpress_phpunit_filters()`, which wraps each
existing child regex in a non-capturing group and joins them with `|`. The
underlying coverage expressions remain intact; only the number of PHPUnit
process launches changes. The shard emits its matrix label through the existing
final harness call, so the timing rollup still sees a normal shard-level
`phpunit-*` row.

`db-short` keeps its two child invocations and synthetic group row so the
unprofiled DB run and profiled DB diagnostic remain separately visible.

## Non-Goals

- Changing WordPress test coverage, WordPress source, or PHPUnit semantics.
- Combining process-isolated deferred reconnect shards.
- Combining DB and DB-profile diagnostics.
- Changing Docker image contents, runtime artifacts, production build modes,
  MyLite runtime behavior, native storage behavior, SQL behavior, or ownerless
  concurrency behavior.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout,
ownerless-concurrency, or WordPress application behavior changes. CI still runs
the same filter coverage against Release MyLite PHP artifacts and a MinSizeRel
MariaDB embedded archive.

## Build And Performance Impact

The expected benefit is lower PHPUnit shell overhead on the existing WordPress
matrix critical path. The tradeoff is coarser timing for the small grouped
filters: CI now reports the grouped matrix label rather than per-child labels
for those already-proven short shards. The DB/profile group remains detailed
because it carries diagnostic mysqli profile data.

Based on run `27968595768`, this targets the critical block short shard and
the remaining-short group, where repeated child launches contributed several
seconds of wrapper overhead. CI remains the authority for final timing because
GitHub runner Docker and PHPUnit startup variance is significant.

## Test And Verification Plan

- Syntax-check the extracted WordPress shard shell block.
- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run focused production CTest coverage for the CI production-build audit and
  WordPress timing rollup.
- Run the production format check.
- Run `git diff --check`.
- Let pushed CI provide the full production timing for combined short filters.

## Acceptance Criteria

- The workflow has `combine_wordpress_phpunit_filters()` in the WordPress
  shard step.
- Same-mode short shards use one matrix-level `phpunit-*` timing label instead
  of child `phpunit-*` labels.
- `db-short` still emits `phpunit-db`, `phpunit-db-profile`, and the DB group
  row.
- The production-build audit rejects reintroduced child timing labels for the
  optimized short shards.
- WordPress timing jobs still require Release MyLite PHP builds and a
  MinSizeRel MariaDB embedded archive.

## Verification Results

Local verification completed:

- extracted WordPress shard shell block with `bash -n`: passed;
- `bash -n tools/check-ci-production-builds`: passed;
- `tools/check-ci-production-builds`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup`: passed;
- `bash -n tools/wordpress-phpunit-timing-rollup-test`: passed;
- `tools/wordpress-phpunit-timing-rollup-test`: passed;
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed, 2/2 in 4.12s;
- `cmake --build --preset format-check-prod`: passed;
- `git diff --check`: passed.

Pushed CI provides the first full production timing for combined short filters.

## Risks

- Coarser timing can hide which child inside a short combined shard grew. If a
  combined shard becomes a new tail, split it again or run a diagnostic branch
  with child labels restored.
- Regex alternation must preserve the old child coverage exactly. The helper
  wraps each old regex without editing the expression body to keep that risk
  bounded.
