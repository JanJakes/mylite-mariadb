# WordPress Docker Buildx Cache

## Problem

After `c9005932`, the WordPress timing rollup directly reports fixed shard
overhead. The green CI run `27897843305` showed that the critical WordPress
shard was not dominated by PHPUnit execution:

- critical shard: `deferred-reconnect-baseline-restored`;
- critical shard total: `120s`;
- artifact download: `7s`;
- artifact extract: `2s`;
- Docker image setup: `73s`;
- PHPUnit shell real time: `37.895s`;
- non-PHPUnit-shell time: `82.105s`;
- estimated WordPress workflow critical path: `194s`;
- sum of all shard Docker image setup rows: `599s`.

Most shard Docker image builds were `26-35s`, with one `73s` outlier on the
critical path. Query/canonical still had the largest test body (`79.971s`
shell real), but its total shard path was `114s`, below the Docker-heavy
critical shard. The next high-impact slice is therefore reducing per-shard
Docker image setup, not adding more test shards.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` previously built the same WordPress PHPUnit
  Docker image once in the setup job and again in every shard job by invoking
  `tools/wordpress-phpunit-mysqli-mylite` with `MYLITE_WORDPRESS_PHASE` set to
  `docker-image`.
- `tools/wordpress-phpunit-mysqli-mylite` previously embedded the Dockerfile
  as a heredoc in `build_image()`, which prevented the workflow from using
  `docker/build-push-action` directly against the same Dockerfile.
- The Docker GitHub Actions cache backend documentation says the `gha` cache is
  intended for GitHub Actions, exposes a `scope` option, and is populated
  automatically by `docker/build-push-action` without manually exposing cache
  URL/token environment variables.
- Docker's `setup-buildx-action` documentation says its default
  `docker-container` driver can export cache through a BuildKit container.
  Local smoke testing confirmed the default local Docker driver cannot export
  BuildKit cache, so CI must explicitly set up Buildx before using `type=gha`.

References:

- <https://docs.docker.com/build/cache/backends/gha/>
- <https://github.com/docker/setup-buildx-action>
- <https://github.com/docker/build-push-action>

## Design

Move the WordPress PHPUnit image definition into
`tools/wordpress-phpunit.Dockerfile`. Keep the local harness behavior by
changing `build_image()` to call `docker build -f
tools/wordpress-phpunit.Dockerfile tools`.

In CI, replace the harness-driven `docker-image` phase with explicit Buildx
steps:

- mark the Docker-image timing start;
- run `docker/setup-buildx-action@v4`;
- run `docker/build-push-action@v7` with:
  - `context: ./tools`,
  - `file: ./tools/wordpress-phpunit.Dockerfile`,
  - `load: true`,
  - `tags: mylite-wordpress-phpunit:php83`,
  - `cache-from: type=gha,scope=wordpress-phpunit-php83`,
  - setup job only: `cache-to: type=gha,mode=max,scope=wordpress-phpunit-php83`;
- append the elapsed seconds under the existing `docker-image` and
  `docker-image-${{ matrix.shard }}` timing labels.

Keeping the same timing labels means `tools/wordpress-phpunit-timing-rollup`
does not need a new metric shape. Buildx setup, cache restore, image build,
setup-job cache export, and Docker image load all remain included in
`wordpress_docker_build_seconds`, so the benchmark remains honest. Shard jobs
restore from the setup-populated cache but do not export back to the same cache
scope concurrently.

`tools/check-ci-production-builds` now guards the Dockerfile path, Buildx
actions, cache scope, image tag, and timing rows.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, storage, directory, native recovery, or
WordPress application behavior changes. CI still runs the same WordPress tests
inside the same PHP 8.3 image contents.

## Directory And Lifecycle Impact

No database-directory behavior changes. The Docker image definition moves from
a Bash heredoc into a first-party Dockerfile. WordPress runtime artifacts and
temporary MyLite database directories are unchanged.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, or ownerless concurrency
behavior changes.

## Build, Size, License, And Dependency Impact

No compiled MyLite binary-size or license changes. CI adds two Docker-owned
GitHub Actions already published under the Docker organization:
`docker/setup-buildx-action@v4` and `docker/build-push-action@v7`.

The first run for a new cache scope may still pay the cold image build and
cache export cost. Subsequent setup and shard jobs should reuse the GitHub
Actions BuildKit cache for the PHP/GD/ZIP toolchain layers.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run the production CTest wrappers for `tools.ci-production-builds` and
  `tools.wordpress-phpunit-timing-rollup`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first Buildx/cache timing evidence for setup and shard
  Docker image rows.

## Acceptance Criteria

- CI uses `docker/setup-buildx-action@v4` and
  `docker/build-push-action@v7` for WordPress setup and shard Docker image
  builds.
- The image is loaded under `mylite-wordpress-phpunit:php83` before subsequent
  harness phases run.
- The final timing summary still contains `docker-image` and
  `docker-image-${{ matrix.shard }}` rows with
  `wordpress_docker_build_seconds`.
- The production audit fails if the Buildx cache settings, Dockerfile path, or
  timing append rows are removed.
- CI timing remains production-build based for MariaDB and MyLite artifacts.

## Verification

- `bash -n tools/check-ci-production-builds`: passed.
- `bash -n tools/wordpress-phpunit-mysqli-mylite`: passed.
- `tools/check-ci-production-builds`: passed.
- `docker buildx build --check -f tools/wordpress-phpunit.Dockerfile tools`:
  passed with no warnings.
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed two tests.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.
- CI run `27898291213` for `29e8cfcb` passed on the first attempt. That
  first Buildx run seeded/exported the cache and was not a wall-clock win:
  setup rose to `133s`, the critical shard was
  `non-isolated-query-canonical` at `113s`, estimated WordPress critical path
  was `246s`, and total Docker setup rows fell from the previous `599s` to
  `482s`.
- Rerunning the same commit after cache seeding passed and provided the first
  hot-cache timing sample: setup was `67s`, the critical shard was
  `non-isolated-content-data` at `117s`, estimated WordPress critical path was
  `184s`, and total Docker setup rows fell to `447s`. The critical shard
  breakdown was `8s` artifact download, `3s` artifact extract, `36s` Docker
  image setup, and `70s` PHPUnit total.

## Risks

- BuildKit cache export/import can be slower than a warm local Docker cache on
  a single runner. The branch currently uses separate shard jobs, so the
  relevant comparison is cross-job wall time, not a single warmed runner.
- GitHub Actions cache size and eviction policy can affect hit rate. The
  timing rows remain in place so misses are visible instead of being assumed
  away.
- The first CI run may seed the cache rather than show the full benefit.
