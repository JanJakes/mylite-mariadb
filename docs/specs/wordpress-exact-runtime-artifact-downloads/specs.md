# WordPress Exact Runtime Artifact Downloads

## Problem

WordPress PHPUnit CI now builds the runtime Docker image in a parallel job and
loads it in each test-only shard. Green run `27972772648` on `b2ec5d7bd`
showed the desired shape: the estimated WordPress critical path fell to `108s`
and engine probes stayed stable. The follow-up docs-only run `27973386108` on
`3bbe9887b` remained green but exposed a noisy artifact transport outlier:
shard `non-isolated-rest-content-support` spent `116s` in the single merged
runtime-artifact download action, pushing the measured critical path to `206s`
even though its PHPUnit phase was only `23s`.

The current single wildcard download cannot show whether the outlier came from
the runtime root artifact, the runtime-image artifact, artifact enumeration, or
the merge path. The next slice must preserve the accepted runtime-image
artifact design while making shard transfer cost attributable and less
dependent on wildcard artifact matching.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source or SQL behavior.
- GitHub Actions already publishes two separate shard prerequisites:
  `wordpress-phpunit-runtime` from the setup job and
  `wordpress-phpunit-runtime-image` from the parallel runtime-image job.
- The previous shard job downloaded both artifacts with
  `pattern: wordpress-phpunit-runtime*` and `merge-multiple: true`, then
  recorded one `artifact-download-<shard>` timing row.
- `tools/wordpress-phpunit-timing-rollup` already builds shard critical path
  from per-shard timing labels and can keep a combined artifact-download
  metric while exposing finer root/image components.

## Design

- Replace the shard wildcard artifact download with two exact artifact
  downloads:
  `name: wordpress-phpunit-runtime` and
  `name: wordpress-phpunit-runtime-image`.
- Keep both artifacts in `build/wordpress-phpunit-runtime-artifact` so
  extraction and `docker load` paths remain unchanged.
- Record `artifact-runtime-download-<shard>` rows with:
  `wordpress_artifact_download_seconds`,
  `wordpress_runtime_artifact_download_seconds`,
  runtime tarball bytes, database-baseline tarball bytes, and
  `wordpress_total_seconds`.
- Record `artifact-image-download-<shard>` rows with:
  `wordpress_artifact_download_seconds`,
  `wordpress_runtime_image_artifact_download_seconds`,
  runtime-image tarball bytes, and `wordpress_total_seconds`.
- Keep the rollup's existing
  `wordpress_shard_critical_path_artifact_download_seconds` as the combined
  root-plus-image transfer cost so prior critical-path comparisons remain
  readable.
- Add critical-path component rows for root download and runtime-image download
  so a future outlier identifies the transfer source.
- Update the production CI audit to require exact artifact names, reject the
  stale runtime wildcard pattern, and require the new split timing labels.

## Non-Goals

- Changing WordPress PHPUnit filters, shard membership, process-isolated
  behavior, or database baseline contents.
- Changing Docker image contents, MyLite PHP extension behavior, SQL behavior,
  mysqli behavior, native storage, redo, checkpoint, recovery, or ownerless
  concurrency behavior.
- Removing artifact transport from CI. This slice only makes the current
  accepted runtime-image artifact path more attributable and avoids wildcard
  artifact matching in shards.

## Compatibility Impact

No application compatibility behavior changes. CI still runs the same
WordPress PHPUnit surface against Release MyLite PHP artifacts and a MinSizeRel
MariaDB embedded archive.

## Directory And Lifecycle Impact

No durable MyLite directory layout changes. Shards continue to restore the
prepared WordPress MyLite database baseline into external temporary database
directories before running PHPUnit.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, locking, or ownerless
page-version behavior changes.

## Build And Performance Impact

Exact artifact names avoid wildcard artifact enumeration and merge semantics on
the shard hot path. The byte volume is unchanged. The expected improvement is
lower variance and stronger evidence when GitHub artifact transfer dominates a
green run. If the runtime-image artifact remains the dominant transfer, the
next performance slice can compare artifact image load against a cached shard
Buildx fallback using evidence from the new split metrics.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/check-ci-production-builds`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run focused production CTest coverage for the CI production-build audit and
  WordPress timing rollup.
- Run the production format check.
- Run `git diff --check`.
- Let pushed CI provide production timing for the exact-download shape.

## Acceptance Criteria

- Shards download `wordpress-phpunit-runtime` and
  `wordpress-phpunit-runtime-image` by exact artifact name.
- Shards no longer use `pattern: wordpress-phpunit-runtime*` for runtime
  artifacts.
- The timing rollup reports combined artifact-download critical-path seconds,
  plus root and runtime-image artifact download seconds for the critical shard.
- Aggregate rollup rows report total runtime-root artifact download seconds,
  runtime-image artifact download seconds, and their tarball byte sums.
- The production audit rejects the stale wildcard runtime artifact path.

## Verification Results

Local verification on 2026-06-22:

- `bash -n tools/check-ci-production-builds`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/check-ci-production-builds`: passed.
- `tools/wordpress-phpunit-timing-rollup-test`: passed.
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed, 2/2 tests in 6.61s.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Risks

- Splitting one download action into two exact download actions may not reduce
  total transfer time when GitHub artifact service latency dominates.
- Sequential exact downloads can still produce a slow shard if either artifact
  transfer stalls, but the new timing rows identify which artifact caused it.
