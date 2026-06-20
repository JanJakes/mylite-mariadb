# WordPress PHPUnit Artifact Timing

## Problem Statement

The WordPress PHPUnit CI path now separates build/setup work from the matrixed
test-only shards and requires production `Release`/`MinSizeRel` build products
before publishing timing rows. That made PHPUnit body, process-child, database
prep, dependency, build, and performance-probe timings visible, but the split
introduced a runtime artifact handoff whose wall time is not represented in the
shared timing summary.

This slice adds timing and size rows for the WordPress runtime artifact pack,
upload, download, and extract phases so CI branch/main comparisons can see
whether the split moved cost into artifact transfer instead of test execution.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not alter
  MariaDB source, MyLite runtime code, native storage, SQL behavior, or PHP
  extension behavior.
- `.github/workflows/ci.yml` already times WordPress Docker image, fetch,
  build, dependencies, database prep, performance probe, and PHPUnit test-only
  phases through `tools/wordpress-phpunit-mysqli-mylite`.
- The setup job packs `build/wordpress-phpunit-runtime.tar.gz` and
  `build/wordpress-phpunit-db-baseline.tar.gz`, uploads them through
  `actions/upload-artifact`, and uploads the setup timing summary afterwards.
- The shard job downloads `wordpress-phpunit-runtime`, extracts both tarballs,
  verifies the runtime manifest, then records test-only timing rows through the
  harness.
- `tools/wordpress-phpunit-timing-rollup` aggregates phase rows into the final
  timing summary, but it does not know artifact handoff metrics.
- `tools/check-ci-production-builds` guards production timing steps and
  artifact names/paths, so new timing markers should be audited there.

## Scope And Non-Goals

In scope:

- Add a small first-party timing-summary append helper usable by CI shell steps
  outside the main WordPress harness.
- Record setup-side artifact pack seconds plus runtime and database-baseline
  tarball byte sizes.
- Bracket the upload action with setup-side start/record steps so upload wall
  time appears in the setup timing summary.
- Bracket the download action with shard-side start/record steps so per-shard
  download wall time appears in each shard timing summary.
- Record per-shard extract seconds plus downloaded tarball byte sizes.
- Teach the rollup to emit aggregate artifact timing and size rows.
- Extend CI production-build audit checks for the new timing helper and
  workflow markers.

Out of scope:

- Changing the artifact contents, compression level, shard matrix, PHPUnit
  filters, WordPress test behavior, or MyLite/MariaDB runtime behavior.
- Claiming an optimization has reduced CI wall time. This is visibility for the
  next optimization decision.

## Design

Add `tools/wordpress-phpunit-append-timing` with the same Markdown table shape
as the main harness:

```sh
tools/wordpress-phpunit-append-timing \
  --label artifact-pack \
  wordpress_artifact_pack_seconds=... \
  wordpress_total_seconds=...
```

The helper takes the summary path from `MYLITE_WORDPRESS_TIMING_SUMMARY_PATH`
by default and preserves the same `| label | metric | value |` format already
consumed by `tools/wordpress-phpunit-timing-rollup`.

Setup-side pack timing uses one shell step around manifest generation,
checksum generation, and both `tar -czf` commands. Upload timing uses a marker
file written immediately before `actions/upload-artifact@v4` and a following
`if: always()` shell step that appends the elapsed seconds before the setup
timing summary artifact is uploaded.

Shard-side download timing uses the same marker-file pattern around
`actions/download-artifact@v4`. Extract timing wraps removal, both tar
extracts, checksum verification, and manifest SHA/ref checks.

The rollup treats artifact seconds and bytes as phase metrics. Each artifact
row also records `wordpress_total_seconds` for the existing timed-phase total.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, WordPress behavior, or MySQL/MariaDB
compatibility behavior changes. The slice only changes CI diagnostics.

## Directory And Lifecycle Impact

No database-directory behavior changes. The helper appends Markdown rows under
`build/wordpress-phpunit-reports/`, and marker files live under `build/`.
The existing temporary `/tmp/mylite-wordpress-tests*.mylite` directories and
runtime artifacts are unchanged.

## Native Storage Impact

No native storage behavior or format changes.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes. The
new helper is a Bash script using existing POSIX/GNU shell utilities already
used by the workflow.

## Test And Verification Plan

- Run `bash -n` for the new helper, CI audit, WordPress harness, and timing
  rollup scripts.
- Add a focused helper smoke test that writes a temporary timing summary and
  verifies Markdown escaping plus header creation.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R 'tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)' --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check` and `git diff --cached --check`.
- Watch CI after the commit to confirm the final WordPress timing summary
  includes artifact rows.

## Verification Results

- `bash -n tools/wordpress-phpunit-append-timing
  tools/wordpress-phpunit-append-timing-test
  tools/check-ci-production-builds
  tools/wordpress-phpunit-mysqli-mylite
  tools/wordpress-phpunit-timing-rollup
  tools/wordpress-phpunit-timing-rollup-test` passed.
- `tools/wordpress-phpunit-append-timing-test` passed.
- `tools/wordpress-phpunit-timing-rollup-test` passed.
- `tools/check-ci-production-builds` passed.
- `cmake --preset prod` refreshed the production CTest build tree.
- `ctest --preset prod -R
  'tools\.(ci-production-builds|wordpress-phpunit-(timing-rollup|append-timing))'
  --output-on-failure` passed 3/3 tests in 5.94 seconds.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed before staging.
- The first green CI run after the artifact timing slice (`55dff596`) confirmed
  setup-side `artifact-pack` and `artifact-upload` rows, but the per-shard
  timing summaries did not retain `artifact-download` or `artifact-extract`
  rows. The shard job runs a harness `docker-image` phase after artifact
  extraction, and the harness intentionally cleared its timing summary at the
  start of that phase for standalone runs. The follow-up fix adds
  `MYLITE_WORDPRESS_PRESERVE_TIMING_SUMMARY=1` to CI shard jobs and guards the
  harness cleanup so workflow-level download/extract rows survive into each
  uploaded shard timing summary.

## Acceptance Criteria

- Setup timing summary includes artifact pack and upload rows.
- Each shard timing summary includes artifact download and extract rows.
- Final rollup emits aggregate artifact pack/upload/download/extract seconds
  and runtime/database-baseline tarball bytes.
- CI audit fails if the artifact timing helper or workflow markers are removed.
- Existing production build guards remain in the timing-producing steps.

## Risks And Follow-Up

- Upload/download wall time is measured by bracketing the GitHub action, so it
  includes small scheduler overhead around the action. That is acceptable for
  branch/main comparison because the same workflow shape measures both sides.
- If CI shows artifact transfer dominates, the next optimization should reduce
  runtime artifact contents or avoid transferring build directories that shards
  do not need.
