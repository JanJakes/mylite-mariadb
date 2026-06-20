# WordPress PHPUnit Parallel Shards

## Problem

The WordPress PHPUnit CI job now exposes build, dependency, perf-probe, and
test-only timings, but all PHPUnit shards still run sequentially after the
production build. The latest green production timing rollup showed about
`631s` of test-only shell time, with about `564s` in the three non-isolated
WordPress shards. The process-isolated child-process runtime was about `45s`
total, and PHP process startup was about `25ms`, so the highest-impact CI
performance lever is wall-time parallelism for the already split test shards,
not another process-startup micro-optimization.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` runs the WordPress harness with
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`, and production guards before
  each timing-sensitive step.
- `tools/wordpress-phpunit-mysqli-mylite` already supports isolated phases:
  `docker-image`, `fetch`, `build-php`, `dependencies`, `prepare-db`,
  `perf-probe`, and `phpunit`.
- The harness checks production freshness in `prepare-db`, `perf-probe`, and
  `phpunit` by validating the MariaDB embedded `MinSizeRel` cache, the
  `Release` CMake cache, and built PHP extension artifacts.
- The harness requires `MYLITE_WORDPRESS_DB_DIR` and
  `MYLITE_WORDPRESS_DB_BASELINE_DIR` to share a parent when the database lives
  outside the repository. That rule lets a CI shard mount one external parent
  directory into the Docker container and restore a baseline before testing.
- Local warmed artifact sizes are acceptable for handoff: roughly `403M` for
  the WordPress checkout, `160M` for the MariaDB embedded build, `24M` for the
  PHP extension build, and small Composer/PHPUnit directories.
- GitHub artifact upload should not be trusted to preserve executable bits,
  hidden files, or symlink metadata for an unpacked tree. A tarball handoff is
  safer for `vendor/bin/phpunit`, WordPress `.git`, and CMake build outputs.
- Cross-job artifact extraction can make mtime-based freshness checks compare
  freshly checked-out source files with older build artifacts. The shard jobs
  therefore use a runtime manifest instead of source mtimes as their handoff
  proof.

## Design

Split the WordPress PHPUnit workflow into three CI jobs while preserving the
old externally visible status name on the final aggregate job:

- `wordpress-phpunit-setup` builds the Docker image, fetches WordPress, builds
  the production MyLite PHP artifacts, installs dependencies, prepares the
  baseline database, runs the mysqli perf probe, then uploads tarballs for the
  runtime tree and prepared database baseline.
- `wordpress-phpunit-shard` is a matrix job. Each shard downloads and extracts
  those tarballs, rebuilds the Docker image locally, verifies the production
  build directories and runtime manifest, restores its own external MyLite
  database directory from the baseline, and runs exactly one existing test-only
  filter.
- `wordpress-phpunit` keeps the name `wordpress-phpunit-mysqli-mylite`, runs
  after setup and all shards, combines timing-summary artifacts, appends the
  deterministic rollup rows, uploads the combined summary under the original
  artifact name, and fails if setup or any shard failed.

The shard matrix covers the same PHPUnit partition as the sequential workflow:

- `phpunit-db`;
- `phpunit-db-profile`;
- `phpunit-deferred-reconnect-baseline-restored`;
- `phpunit-deferred-reconnect-skip-install`;
- `phpunit-deferred-reconnect-ui`;
- `phpunit-non-isolated-rest`;
- `phpunit-non-isolated-query-theme-block-token`;
- `phpunit-non-isolated-remaining`.

A later timing slice split `phpunit-non-isolated-rest` into
`phpunit-non-isolated-rest-controller` and
`phpunit-non-isolated-rest-other` after production CI showed REST had become
the longest individual shard. The aggregate job still treats both labels as
non-isolated PHPUnit shards.

A follow-up split divided `phpunit-non-isolated-query-theme-block-token` into
`phpunit-non-isolated-query-theme` and `phpunit-non-isolated-block-token` after
REST was no longer the critical path.

Each shard runs with its own `/tmp/mylite-wordpress-tests.mylite` path in a
separate GitHub runner, so native MyLite database files are not shared across
parallel jobs. The setup job also uses `/tmp` for the prepared baseline so the
existing external-database guard still proves CI timing is not accidentally
using repository-local database storage.

The setup job writes `wordpress-phpunit-runtime-manifest.txt` with the GitHub
SHA, WordPress ref and checked-out SHA, MariaDB embedded build type, and MyLite
PHP extension build type. It also writes a SHA256 manifest over the MariaDB
embedded archive, `libmylite.a`, both PHP extensions, the PHP wrapper, extension
configuration, PHPUnit binary, WordPress test config, and the metadata file.
Shard jobs verify the manifest immediately after extraction and pass
`MYLITE_WORDPRESS_TRUST_RUNTIME_MANIFEST=1` to the harness so the host-side
freshness check uses this manifest instead of cross-job mtimes. Local and setup
runs keep the existing source-mtime freshness checks. For `phpunit` phases with
`MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`, the harness accepts a baseline-only
handoff and restores `MYLITE_WORDPRESS_DB_DIR` from
`MYLITE_WORDPRESS_DB_BASELINE_DIR` immediately before executing PHPUnit; other
phases still require the active prepared database directory to exist up front.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, storage format, or WordPress
behavior changes. This is a CI orchestration and diagnostic-output slice. The
same WordPress filters remain the compatibility authority.

## Directory And Lifecycle Impact

No product directory-layout change. CI transient tarballs contain a prepared
WordPress MyLite database baseline and are extracted into per-job temporary
directories. PHPUnit shard phases restore from that baseline into their own
MyLite-owned database directory before executing tests.

## Native Storage Impact

No native storage-engine behavior changes. The shard jobs exercise ordinary
embedded mysqli MyLite databases, not ownerless mode.

## Build, Size, License, And Dependencies

No compiled code or dependency changes. CI now uploads/downloads compressed
runtime and database-baseline tarballs to avoid duplicating the MariaDB embedded
and PHP extension builds across shards. The precompressed runtime artifact uses
`compression-level: 0` for GitHub artifact upload so the tarball is not
compressed again by the artifact layer. The Docker image is still built per
runner because saving and restoring the full image is expected to be larger and
less useful than its current roughly `30s` build cost.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R 'tools\.ci-production-builds|tools\.wordpress-phpunit-timing-rollup' --output-on-failure`.
- Run `git diff --check`.
- Run `cmake --build --preset format-check-prod`.
- Let CI prove the artifact handoff and parallel shard matrix under production
  build guards.

## Acceptance Criteria

- The old `wordpress-phpunit-mysqli-mylite` status name belongs to the final
  aggregate job, not to a setup-only job.
- Setup and shard jobs reject stale or non-production MyLite/MariaDB build
  directories before reporting timings; shard jobs verify the runtime manifest
  instead of relying on artifact mtimes.
- Every PHPUnit shard uses `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1` and an
  external MyLite database directory.
- The final timing artifact keeps the original
  `wordpress-phpunit-timing-summary` name and includes rollup rows across setup
  and all shard summaries.
- A shard failure fails the final aggregate job.

## Verification Results

Local verification on 2026-06-20:

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `bash -n tools/check-ci-production-builds` passed.
- `tools/check-ci-production-builds` passed and now requires the setup, matrix
  shard, runtime-manifest, and aggregate timing-summary structure.
- `ctest --preset prod -R 'tools\.ci-production-builds|tools\.wordpress-phpunit-timing-rollup' --output-on-failure`
  passed.
- `git diff --check` passed.
- `cmake --build --preset format-check-prod` passed.

## Risks And Follow-Up

Artifact upload/download can move the wall-time bottleneck if compression is
too expensive on GitHub runners. The first CI run after this slice is the
authority for that tradeoff. If artifact transfer dominates, the next bounded
optimization should either trim the handoff contents or move only the prepared
runtime pieces needed by a `phpunit` phase.
