# WordPress Zstd Runtime Artifact

## Problem

The WordPress PHPUnit CI fanout now separates production setup, runtime
artifact transport, and test-only shard execution. The remaining fixed shard
overhead still includes repeated runtime artifact download and extraction
before any PHPUnit body runs. Recent timing specs record compressed runtime
payloads around 75-82 MiB and extract/download work multiplied across the
visible shard matrix.

The current transport uses gzip tarballs. That is compatible but slower to
compress and decompress than zstd for CI-sized binary/source snapshots on
GitHub's Ubuntu runners.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `.github/workflows/ci.yml` already stages a minimal
  `build/wordpress-phpunit-runtime-root`, verifies a manifest inside that
  staged root, and uploads `wordpress-phpunit-runtime` with artifact action
  compression disabled.
- Shards already verify the runtime manifest, GitHub SHA, WordPress ref,
  MinSizeRel MariaDB archive, and Release MyLite PHP build before running
  PHPUnit.
- The timing rollup already tracks runtime and database baseline artifact byte
  counts through `wordpress_artifact_pack_*_tar_bytes` and
  `wordpress_artifact_extract_*_tar_bytes`.

## Design

Keep the existing runtime artifact contents and manifest checks, but change the
transport files from gzip tarballs to zstd-compressed tar streams:

- setup writes `wordpress-phpunit-runtime.tar.zst` and
  `wordpress-phpunit-db-baseline.tar.zst` with `tar | zstd -T0 -3`;
- shards extract with `zstd -dc | tar`;
- byte-count metric names remain unchanged so historical rollups stay
  comparable as "compressed tar transport bytes";
- the production-build audit rejects stale `.tar.gz` workflow references and
  requires the zstd pack and extract commands.

## Non-Goals

- Changing WordPress PHPUnit filters, shard grouping, test selection, or
  process-isolated settings.
- Changing SQL behavior, mysqli behavior, public C API behavior, native
  storage, directory lifecycle, ownerless locking, redo, checkpoint, or
  recovery behavior.
- Changing runtime artifact contents, manifest hashing, production build
  modes, Docker images, or timing-rollup aggregation semantics.

## Compatibility Impact

No application compatibility behavior changes. CI still runs the same
WordPress tests against the same Release MyLite PHP artifacts and MinSizeRel
MariaDB embedded archive.

## Directory And Lifecycle Impact

No MyLite durable directory layout changes. Shards still restore the prepared
WordPress MyLite database baseline into an external temporary directory before
the test-only phase.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, locking, or ownerless
page-version behavior changes.

## Build And Performance Impact

The expected benefit is lower setup compression time, lower shard extraction
time, and potentially smaller artifact download bytes. CI timing remains the
authority because GitHub artifact transport and runner cache state vary across
runs. The artifact action still uses `compression-level: 0`, so compression is
owned by the explicit zstd step rather than nested action compression.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run focused production CTest coverage for the CI production-build audit and
  WordPress timing rollup.
- Smoke a local zstd tar round trip.
- Run the production format check.
- Run `git diff --check`.
- Let the pushed CI run provide production timing for the zstd artifact
  transport.

## Acceptance Criteria

- The workflow uploads and extracts `.tar.zst` runtime and database baseline
  artifacts.
- The setup job records the same runtime and database baseline compressed byte
  metrics for the zstd files.
- The shard job records the same extraction byte metrics for the zstd files.
- The production-build audit fails if the workflow reintroduces gzip runtime
  artifacts or omits the zstd pack/extract commands.
- Existing timing rollup tests still pass unchanged.

## Verification Results

Local verification on 2026-06-22:

- `bash -n tools/check-ci-production-builds`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup`: passed.
- `bash -n tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/check-ci-production-builds`: passed.
- `tools/wordpress-phpunit-timing-rollup-test`: passed.
- A local `tar | zstd -T0 -3` and `zstd -dc | tar` round trip passed with a
  147-byte compressed smoke artifact.
- `ctest --preset prod -R '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed, 2/2 tests in 6.24s.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

CI must provide the first full production timing for the zstd artifact
transport.

## Risks And Follow-Up

- GitHub runner artifact upload/download time can dominate compression format
  differences on noisy runs. Compare `wordpress_artifact_download_seconds`,
  `wordpress_artifact_extract_seconds`, compressed byte totals, and critical
  shard non-PHPUnit overhead together.
- If a future non-Ubuntu WordPress job is added, it must provide `zstd` before
  reusing this workflow shape.
