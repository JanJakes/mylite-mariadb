# WordPress Runtime Image Artifact

## Problem

The WordPress PHPUnit CI job now separates production setup from parallel
test-only shards, uses zstd runtime artifacts, and combines short same-mode
PHPUnit filters. Green run `27970212605` on `ecd6b5342` still reported fixed
shard setup as a large cost:

- `wordpress_docker_build_seconds`: `599s` summed across setup and shards;
- critical shard: `non-isolated-remaining-admin-site`;
- critical shard Docker image phase: `36s`;
- estimated WordPress workflow critical path: `140s`;
- artifact download: `96s`;
- artifact extract: `15s`.

The shard phase does not need to run Buildx. Setup already builds the
test-only runtime Dockerfile and has the runtime image loaded in a production
job. Rebuilding or materializing that image independently in every shard adds
high-variance work before PHPUnit starts.

The first artifacted-image run, `27971579104` on `f6a71cf62`, proved the
image artifact path but showed that saving and uploading the image inside the
main setup job moved cost onto the setup critical path:

- shard Docker work fell from `599s` to `278s`;
- shard image-load work was `253s`;
- critical shard non-PHPUnit shell time fell from `42.839s` to `31.101s`;
- artifact download rose from `96s` to `213s`;
- setup rose from `67s` to `83s`;
- estimated workflow critical path rose from `140s` to `146s`.

The follow-up shape must keep the shard-side image-load win while moving
runtime image build, save, and upload into a parallel prerequisite job.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `tools/wordpress-phpunit-mysqli-mylite` defaults to
  `mylite-wordpress-phpunit:php83` and accepts
  `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1`, so CI can prepare the Docker image
  outside the harness and keep PHPUnit execution unchanged.
- The setup job builds `tools/wordpress-phpunit.Dockerfile` as the
  build-capable image. A separate runtime-image job builds
  `tools/wordpress-phpunit-runtime.Dockerfile` under
  `mylite-wordpress-phpunit-runtime:php83`.
- The shard job currently runs Buildx against the runtime Dockerfile in every
  matrix job, loads it under `mylite-wordpress-phpunit:php83`, and records that
  work as the `docker-image-<shard>` timing phase.
- The existing runtime artifact already uses explicit zstd compression and
  action-level compression disabled, so adding another `.tar.zst` payload keeps
  compression ownership in workflow shell steps.

## Design

Keep the setup image and the runtime image distinct:

- the setup job continues to use `mylite-wordpress-phpunit:php83` for build,
  dependency, database preparation, and performance-probe phases;
- a parallel `wordpress-phpunit-runtime-image` job builds the runtime Dockerfile
  with `load: true` while keeping the separate tag
  `mylite-wordpress-phpunit-runtime:php83`;
- the runtime-image job saves that loaded image with `docker image save` and
  zstd compression as
  `build/wordpress-phpunit-runtime-image.tar.zst`;
- the runtime-image job uploads `wordpress-phpunit-runtime-image`, separate
  from the setup job's `wordpress-phpunit-runtime` artifact;
- each shard has both setup jobs in `needs`, downloads both artifacts with one
  merged `wordpress-phpunit-runtime*` artifact action, extracts only the
  runtime root and database baseline, then loads the Docker image tarball with
  `docker load`;
- after loading, the shard retags
  `mylite-wordpress-phpunit-runtime:php83` to the harness default
  `mylite-wordpress-phpunit:php83`;
- the shard timing label remains `docker-image-<shard>` so critical-path
  rollups remain comparable, but the rows now also include
  `wordpress_docker_image_load_seconds` and
  `wordpress_docker_image_load_tar_bytes`.

The production-build audit requires the parallel runtime-image job, the
runtime-image artifact tarball, runtime-image pack/upload metrics, shard-side
Docker load metrics, aggregate result checks for the new job, and rejection of
stale per-shard Buildx runtime-image steps.

## Non-Goals

- Changing WordPress PHPUnit filters, shard membership, process-isolated
  settings, or database baseline semantics.
- Changing SQL behavior, mysqli behavior, public C API behavior, native
  storage, redo, checkpoint, recovery, or ownerless concurrency behavior.
- Removing Docker from the WordPress PHPUnit job.
- Claiming a final wall-clock improvement before CI publishes timing for this
  artifact shape.

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

The expected benefit is lower and less variable per-shard Docker setup time
because shards load one setup-produced image artifact instead of running Buildx.
The tradeoff is larger per-shard artifact download. The runtime image build,
save, and upload now run in parallel with the main setup job, and the rollup
uses the larger of main setup time and runtime-image setup time for estimated
workflow critical path. CI timing must compare:

- `wordpress_runtime_docker_cache_seconds`;
- `wordpress_runtime_docker_image_pack_seconds`;
- `wordpress_runtime_docker_image_upload_seconds`;
- runtime artifact upload/download seconds and bytes;
- shard `wordpress_docker_image_load_seconds`;
- critical-path non-PHPUnit shell seconds;
- estimated workflow critical path.

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
- Let pushed CI provide production timing for the runtime image artifact shape.

## Acceptance Criteria

- Runtime-image CI loads the runtime Dockerfile image under
  `mylite-wordpress-phpunit-runtime:php83`.
- The uploaded WordPress runtime-image artifact includes
  `wordpress-phpunit-runtime-image.tar.zst`.
- Shards download the runtime root and runtime-image artifacts through one
  merged artifact action, load that image artifact, retag it to
  `mylite-wordpress-phpunit:php83`, and run the existing test-only harness with
  `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1`.
- The timing rollup reports runtime-image build/pack/upload, shard image-load,
  and parallel setup critical-path metrics.
- The production audit rejects stale per-shard Buildx runtime-image builds.

## Verification Results

Local verification on 2026-06-22:

- `bash -n tools/check-ci-production-builds`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/check-ci-production-builds`: passed.
- `tools/wordpress-phpunit-timing-rollup-test`: passed.
- Local Docker image save/load smoke passed by tagging the previously built
  no-Composer runtime image as `mylite-wordpress-phpunit-runtime:php83`,
  saving it through `docker image save | zstd -T0 -3`, loading it with
  `zstd -dc | docker load`, and retagging it to
  `mylite-wordpress-phpunit:php83`; the compressed image archive was
  `175314926` bytes.
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed, 2/2 tests in 4.81s.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

Follow-up local verification after splitting the runtime image artifact into a
parallel CI job:

- `bash -n tools/check-ci-production-builds`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/check-ci-production-builds`: passed.
- `tools/wordpress-phpunit-timing-rollup-test`: passed.
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed, 2/2 tests in 5.03s.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

Pushed CI run `27972772648` on `b2ec5d7bd` passed and provided the first full
production timing after the runtime-image job split:

- main setup job: `1m34s`; parallel runtime-image job: `34s`;
- measured setup critical path: `56s`, with the runtime-image phase totaling
  `22s`;
- critical shard: `non-isolated-canonical` at `52s`;
- estimated WordPress workflow critical path from measured phases: `108s`,
  down from `146s` on the first artifacted-image run and `140s` before image
  artifacting;
- shard Docker image-load sum: `277s` across 28 shards, replacing the earlier
  per-shard Buildx runtime-image work;
- runtime image artifact: `174742116` compressed bytes, `5s` pack, `2s`
  upload;
- runtime artifact download/extract sums: `112s` download, `20s` extract;
- PHPUnit shell time stayed stable at `698.155s` summed across shards with
  `88.950s` summed shell overhead;
- performance probes stayed in the expected range: PHP process start
  `28.421ms`, mysqli explicit embedded open `93.330ms`, explicit close
  `27.120ms`, active runtime reconnect `2.044ms`, `SELECT 1` `1533.460`
  ops/s, and autocommit insert `1200.570` ops/s.

The split is therefore accepted as a CI critical-path improvement rather than
an engine performance change. The remaining visible WordPress overhead is
artifact download plus shard-side `docker load`, with the critical shard
showing `4s` download, `1s` extract, and `9s` image load before its `38s`
PHPUnit phase.

## Risks

- The larger runtime artifact can move cost from shard Buildx setup to artifact
  download. The rollout is worthwhile only if the critical path and
  non-PHPUnit shard overhead improve in CI.
- A failed image load now fails every shard early. The setup-side image inspect
  and shard-side inspect/retag checks make that failure explicit before PHPUnit
  starts.
