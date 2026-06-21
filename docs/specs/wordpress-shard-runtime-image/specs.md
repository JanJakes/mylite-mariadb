# WordPress Shard Runtime Image

## Problem

Green CI run `27915469258` on `a30256b3a` proved the REST fanout shape but
showed that filter-only splitting is no longer the main PHPUnit wall-clock
lever. The published timing rollup reported setup action time at `76s`,
critical shard `non-isolated-content-term-taxonomy` at `70s`, and estimated
WordPress critical path at `146s`. The critical shard spent `30s` in PHPUnit
shell real time, `30s` in Docker image setup, `6s` downloading artifacts, and
`3s` extracting artifacts.

The WordPress setup phase needs a build-capable container because it compiles
the MariaDB embedded archive and MyLite PHP artifacts. The shard phase is
test-only: it downloads already-built Release/MinSizeRel artifacts, verifies
the runtime manifest, restores a prepared database baseline, and runs PHPUnit.
Reusing the setup image for every shard therefore makes every shard load a
CMake/Ninja/GCC/bison build environment it does not use.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not touch
  MariaDB source.
- `.github/workflows/ci.yml` runs the WordPress setup job in production mode:
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`,
  `build/wordpress-php-embedded-prod`, and
  `build/wordpress-mariadb-embedded`.
- `tools/wordpress-phpunit-mysqli-mylite` accepts
  `MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1`; CI can prebuild or preload the
  Docker image and keep the harness test phase unchanged.
- The harness default image name is `mylite-wordpress-phpunit:php83`, so shard
  CI can load a different Dockerfile under the same tag without changing the
  test runner.
- `tools/wordpress-phpunit.Dockerfile` installs build-only packages such as
  `bison`, `build-essential`, `cmake`, `ninja-build`, and `pkg-config`, then
  builds the PHP `gd` and `zip` extensions.

## Design

Keep `tools/wordpress-phpunit.Dockerfile` as the setup image. Add
`tools/wordpress-phpunit-runtime.Dockerfile` as the test-only shard image:

- a `php-ext-builder` stage keeps the same PHP extension build shape as the
  setup Dockerfile so Buildx can reuse the existing setup cache where possible;
- the final image starts from `php:8.3-cli-bookworm`, installs only runtime
  package dependencies needed by PHP, `gd`, `zip`, and the shard harness, and
  copies the built PHP extensions plus their `conf.d` entries;
- shards load the runtime Dockerfile as `mylite-wordpress-phpunit:php83`, the
  same tag the harness already runs;
- the setup job seeds a separate
  `wordpress-phpunit-runtime-php83` GitHub Actions Buildx cache before packing
  the runtime artifact;
- shard jobs read both the runtime cache scope and the setup image cache scope,
  then load the runtime image before running the existing test-only harness.

The production audit now requires the setup Dockerfile for setup, the runtime
Dockerfile for shard jobs, the runtime cache seed, and the distinct runtime
cache scope. It also rejects the shard runtime build step if it points back to
the setup Dockerfile.

## Non-Goals

- Changing SQL, mysqli, WordPress, storage-engine, native recovery, or
  ownerless behavior.
- Changing PHPUnit filters, shard membership, process-isolated settings, or
  prepared database semantics.
- Removing Docker from the WordPress PHPUnit job.
- Claiming a final wall-clock improvement before CI publishes timing for this
  image shape.

## Compatibility Impact

No application compatibility behavior changes. The same WordPress PHPUnit
surface runs against the same Release MyLite PHP artifacts and MinSizeRel
MariaDB embedded archive.

## Directory And Lifecycle Impact

No durable directory layout changes. Shards still unpack the verified runtime
artifact and restore independent external MyLite database directories before
running PHPUnit.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, locking, or ownerless
page-version behavior changes.

## Build, Size, License, And Dependency Impact

The runtime image does not introduce a new external dependency; it is another
Dockerfile built from the same `php:8.3-cli-bookworm` and `composer:2` bases
already used by the setup image. The expected performance effect is lower
per-shard Docker image build/load time by excluding the C/CMake build
toolchain from the final test-only image. The setup job pays a new runtime
cache seed step, and CI timing remains the authority for whether the reduced
per-shard Docker time offsets that setup cost.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Build the runtime Dockerfile locally.
- Smoke the runtime image by loading `gd`, `zip`, `mylite`, and
  `mysqli_mylite` through the existing WordPress PHP wrapper.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.
- Let CI provide the first production timing for runtime-image shards.

## Acceptance Criteria

- Setup CI still builds `tools/wordpress-phpunit.Dockerfile` with the setup
  cache scope and production artifacts.
- Setup CI seeds `wordpress-phpunit-runtime-php83` for
  `tools/wordpress-phpunit-runtime.Dockerfile`.
- Shard CI loads `tools/wordpress-phpunit-runtime.Dockerfile` under
  `mylite-wordpress-phpunit:php83` before running the unchanged test-only
  harness.
- The production audit rejects stale shard builds that use the setup Dockerfile.
- The timing rollup reports `wordpress_runtime_docker_cache_seconds` so the
  setup-side cost of this optimization is visible beside shard Docker timings.

## Verification Results

Local verification on 2026-06-21 used the production workflow and audit files
at `be676a598` plus this slice's edits.

Passed:

- `bash -n tools/check-ci-production-builds`
- `bash -n tools/wordpress-phpunit-mysqli-mylite`
- `docker build -f tools/wordpress-phpunit-runtime.Dockerfile tools -t
  mylite-wordpress-phpunit-runtime-test:php83`
- runtime image smoke loading `gd`, `zip`, `mylite`, and `mysqli_mylite`
  through `build/wordpress-php-embedded-prod/mylite-wordpress-php`
- `tools/check-ci-production-builds`
- `tools/wordpress-phpunit-timing-rollup --input
  build/ci-artifacts/27915469258/wordpress-phpunit-timing-summary/timing-summary.md`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The local Docker image size check reported
`mylite-wordpress-phpunit:php83` at `664177920` bytes and
`mylite-wordpress-phpunit-runtime-test:php83` at `565003838` bytes. A full
local PHPUnit harness smoke was not used as verification because the local
`build/wordpress-mariadb-embedded/libmysqld/libmariadbd.a` artifact is stale
relative to the MariaDB source tree; CI will test the production artifact path
with a freshly checked manifest.

CI run `27916382040` passed on `70fe4caf3` with the runtime-image shard shape.
The published timing rollup reported setup action time at `70s`, including a
new `12s` runtime Docker cache seed. The critical shard moved to
`non-isolated-content-post-template` at `68s`, including `34.160s` PHPUnit
shell real time, `23s` Docker image setup, `7s` artifact download, and `3s`
artifact extract. The estimated WordPress critical path was `138s`, down from
`146s` in the prior green run `27915469258`. Total shard Docker setup fell
from `815s` to `737s`; artifact download and extract stayed effectively flat
at `167s` and `105s` versus prior `168s` and `103s`.

The same CI run reported current process and engine probe values of `14.847ms`
stock PHP process start, `21.367ms` PHP process start with extensions loaded,
`6.520ms` extension-load overhead, `127.032ms` explicit process connect/close,
`95.232ms` in-process connect/close, `0.976ms` active-runtime reconnect,
`3208.580` `SELECT 1` ops/s, `3024.050` transactional insert ops/s,
`2766.380` point-select ops/s, `1979.890` prepared autocommit insert ops/s,
and `1947.930` direct autocommit insert ops/s.

## Risks

- A smaller image can reduce per-shard load time while increasing setup time;
  the net effect must be judged from CI critical-path timing, not local image
  size alone.
- If a WordPress PHPUnit shard unexpectedly shells out to a build-only tool,
  the runtime image would need that specific runtime tool added explicitly.
