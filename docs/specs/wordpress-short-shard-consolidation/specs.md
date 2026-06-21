# WordPress Short Shard Consolidation

## Problem

The split WordPress PHPUnit CI now gives useful test-only timing, but the
latest production run still spends too much runner work on fixed per-shard
setup. CI run `27920287939` on `3488db7c3` reported:

- `wordpress_phpunit_test_shell_real_seconds_sum=707.398`;
- `wordpress_phpunit_test_total_seconds_sum=728.000`;
- `wordpress_docker_build_seconds=822.000`;
- `wordpress_artifact_download_seconds=140.000`;
- `wordpress_artifact_extract_seconds=108.000`;
- `wordpress_artifact_extract_runtime_tar_bytes_sum=5718303324`.

Several non-isolated shards had very short PHPUnit bodies but still paid a
separate artifact download, artifact extract, and Docker image materialization:

- `phpunit-non-isolated-remaining-platform-image`: `6.281s` shell real;
- `phpunit-non-isolated-remaining-ai-connectors`: `6.379s`;
- `phpunit-non-isolated-rest-content-history`: `6.700s`;
- `phpunit-non-isolated-rest-controller-tests-rest-font-icon`: `8.478s`;
- `phpunit-non-isolated-remaining-platform-html-interactivity`: `11.756s`.

The next bounded improvement is to stop paying that fixed setup cost for each
of these very small same-mode shards while preserving per-filter timing rows.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- All five target shards use the same non-isolated WordPress settings:
  reconnect after child enabled, child timing disabled, child script timing
  disabled, mysqli profiling disabled, keepalive enabled, child install skip
  disabled, and baseline restore disabled.
- `.github/workflows/ci.yml` already builds one production runtime artifact in
  `wordpress-phpunit-setup`, then each matrix shard downloads/extracts that
  artifact and runs one test-only `tools/wordpress-phpunit-mysqli-mylite`
  invocation.
- `tools/wordpress-phpunit-timing-rollup` computes critical-path timing by
  matching `artifact-download-*`, `artifact-extract-*`, `docker-image-*`, and
  `phpunit-*` labels.

## Design

Replace five standalone matrix entries with two grouped entries:

- `non-isolated-rest-short` runs the existing REST history filter and the
  existing `Tests_REST` font/icon controller filter;
- `non-isolated-remaining-short` runs the existing remaining
  HTML/interactivity, image, and AI/connectors filters.

Each grouped job still runs each child filter through a separate harness
invocation with the original child timing label:

- `phpunit-non-isolated-rest-content-history`;
- `phpunit-non-isolated-rest-controller-tests-rest-font-icon`;
- `phpunit-non-isolated-remaining-platform-html-interactivity`;
- `phpunit-non-isolated-remaining-platform-image`;
- `phpunit-non-isolated-remaining-ai-connectors`.

That keeps per-filter PHPUnit timing visible in the merged timing summary. The
workflow also writes one synthetic `group-phpunit-<shard>` row after each group
finishes. The rollup uses that synthetic row only for shard critical-path math,
and deliberately excludes it from aggregate PHPUnit and runner-time sums so
child timings are not double-counted.

## Non-Goals

- Changing WordPress test filters, test coverage, or WordPress source.
- Grouping DB, db-profile, or process-isolated deferred reconnect shards.
- Changing Docker image contents, runtime artifacts, or production build
  types.
- Claiming an engine-performance improvement.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout,
ownerless-concurrency, or WordPress application behavior changes. This is a CI
scheduling and timing-attribution change only.

## Build And Performance Impact

The matrix loses three net WordPress shard jobs while preserving the same
child filters. Based on run `27920287939`, the grouped filters represented
about `39.594s` of PHPUnit shell time and at least three avoidable shard setup
payments. Actual savings depend on GitHub runner Docker/cache variance.

The rollup remains conservative for critical-path reporting because grouped
jobs publish a group-level timing row. Aggregate PHPUnit sums remain based on
the original per-filter labels.

## Test Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/check-ci-production-builds`.
- Run focused production CTest coverage for the CI production-build audit and
  WordPress timing rollup.
- Run the production format check.
- Run `git diff --check`.
- Let the next pushed CI run provide the full production timing for the grouped
  shard layout.

## Acceptance Criteria

- The workflow has two grouped short-shard matrix entries and no standalone
  matrix entries for the five grouped child filters.
- Each grouped child filter still emits its original `phpunit-*` timing label.
- The timing rollup accounts for group critical path without double-counting
  synthetic group rows in aggregate timing sums.
- The production-build audit requires the grouped matrix entries and grouped
  timing machinery.
- CI still uses Release MyLite PHP builds and a MinSizeRel MariaDB embedded
  archive for WordPress timings.

## Verification Results

Local verification completed:

```text
bash -n tools/check-ci-production-builds                              # passed
bash -n tools/wordpress-phpunit-timing-rollup                         # passed
bash -n tools/wordpress-phpunit-timing-rollup-test                    # passed
tools/wordpress-phpunit-timing-rollup-test                            # passed
tools/check-ci-production-builds                                      # passed
ctest --preset prod -R '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$' --output-on-failure
                                                                      # passed, 2/2 in 4.68s
cmake --build --preset format-check-prod                              # passed
git diff --check                                                      # passed
```

The next pushed CI run is expected to provide the first full production timing
comparison for the grouped shard layout.

## Risks And Follow-Up

Grouped jobs run child filters sequentially on the same GitHub runner. Each
child still uses a separate harness invocation and restored runtime artifacts,
but filesystem side effects outside the MyLite database directory can now live
on the same runner between grouped child invocations. The selected filters are
small non-isolated CI shards that already use the same keepalive and baseline
settings; the next CI run is the full proof.

The larger fixed costs remain Docker image materialization and runtime artifact
fanout. Further grouping or a different image distribution design can reduce
more runner work after this smaller consolidation proves stable.
