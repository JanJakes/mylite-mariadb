# WordPress Runtime Artifact Slimming

## Problem

The WordPress PHPUnit timing split now separates test bodies from CI setup
work, and green run `27921463527` on `5da376ff4` showed that the remaining
fixed shard overhead is still material:

- `wordpress_shard_phase_count=31`;
- `wordpress_docker_build_seconds=585`;
- `wordpress_artifact_download_seconds=109`;
- `wordpress_artifact_extract_seconds=94`;
- `wordpress_artifact_pack_runtime_tar_bytes=158858467`;
- `wordpress_artifact_extract_runtime_tar_bytes_sum=4924612477`.

The setup job currently tars the full WordPress checkout, PHPUnit tools, full
MariaDB embedded build tree, and full MyLite PHP embedded build tree for every
shard. The test-only shard path verifies production build types and runtime
manifest hashes before running PHPUnit, but it does not need Ninja files,
object files, generated build metadata, or Git object history.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `tools/wordpress-phpunit-mysqli-mylite` in `phpunit` mode requires the
  checked-out WordPress tree, PHPUnit tools, the MyLite PHP wrapper, the
  `mylite` and `mysqli_mylite` PHP extensions, `php-conf.d/mylite.ini`, the
  prepared database baseline, and CMake cache files for production build-type
  checks.
- With `MYLITE_WORDPRESS_TRUST_RUNTIME_MANIFEST=1`, the shard host verifies
  the runtime manifest instead of re-running source freshness checks. The
  manifest already hashes `libmariadbd.a`, `libmylite.a`, both PHP extensions,
  the wrapper, `mylite.ini`, PHPUnit, `wp-tests-config.php`, and the manifest
  metadata.
- Local build-tree inspection showed `build/wordpress-develop/.git/objects`
  at about `218 MiB`, `build/wordpress-mariadb-embedded` at about `160 MiB`,
  and `build/wordpress-php-embedded-prod` at about `24 MiB`. The shard does
  not run Git over the WordPress checkout or build from those directories.

## Design

Assemble a dedicated `build/wordpress-phpunit-runtime-root` before creating
`wordpress-phpunit-runtime.tar.gz`:

- stream-copy the WordPress checkout into the runtime root with tar excludes
  for `.git/objects` and `.git/logs`, then copy PHPUnit tools;
- retain the `.git` directory, index, refs, HEAD, and an empty `objects`
  directory as VCS markers for WordPress tests;
- copy only the MariaDB CMake cache and `libmysqld/libmariadbd.a`;
- copy only the MyLite CMake cache, `libmylite.a`, both PHP extensions, the
  wrapper binary, and `php-conf.d/mylite.ini`;
- copy the runtime manifest and validate it from inside the staged root before
  tarring the staged `build/` tree.

The workflow continues to upload the same two artifact files:
`wordpress-phpunit-runtime.tar.gz` and `wordpress-phpunit-db-baseline.tar.gz`.
Shard extraction and PHPUnit execution remain unchanged.

The timing summary gains `wordpress_artifact_pack_runtime_root_bytes` so the
uncompressed staged payload can be compared with the compressed tar byte size.
The production-build audit requires the slim staging root, the exact retained
runtime files, the staged manifest verification, and the staged-root byte
metric.

## Non-Goals

- Changing WordPress PHPUnit filters, shard grouping, test selection, or
  process-isolated settings.
- Changing SQL behavior, mysqli behavior, public C API behavior, native
  storage, directory lifecycle, ownerless locking, redo, checkpoint, or
  recovery behavior.
- Removing Docker from the WordPress PHPUnit job.
- Replacing the prepared database baseline artifact.
- Claiming final PHPUnit parity before CI publishes after-change timing.

## Compatibility Impact

No application compatibility behavior changes. The same WordPress source tree
and tests run against the same Release MyLite PHP artifacts and MinSizeRel
MariaDB embedded archive. The runtime artifact is a CI transport format only.

## Directory And Lifecycle Impact

No durable MyLite directory layout changes. Shards still restore independent
external MyLite database directories from the verified baseline artifact before
running PHPUnit.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, locking, or ownerless
page-version behavior changes.

## Build, Size, And Performance Impact

The setup job performs an extra local staging pass before compression, but the
largest Git object history is excluded while staging rather than copied and
deleted afterward. The artifact uploaded to all shard jobs should be smaller
because Git object history and unused build-tree files are excluded from the
runtime tarball. The expected benefit is lower per-shard artifact download and
extract cost while preserving production build and manifest checks.

CI timing remains the performance authority. Local size inspection only proves
that the excluded directories are not required inputs for the shard harness.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup`.
- Run `bash -n tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/wordpress-phpunit-timing-rollup-test`.
- Run `tools/check-ci-production-builds`.
- Run focused production CTest coverage for the CI production-build audit and
  WordPress timing rollup.
- Run the production format check.
- Run `git diff --check`.
- Let the pushed CI run provide production timing for the slim artifact.

## Acceptance Criteria

- The setup job creates `build/wordpress-phpunit-runtime-root` and tars from
  that staged root rather than from the full build tree.
- Shards still extract paths at `build/wordpress-develop`,
  `build/wordpress-phpunit-tools`, `build/wordpress-mariadb-embedded`, and
  `build/wordpress-php-embedded-prod`.
- The staged root contains the production CMake caches and manifest-hashed
  runtime artifacts needed by the shard guards.
- The staged root excludes WordPress Git object history.
- The staged-root manifest check passes before upload.
- The timing rollup reports `wordpress_artifact_pack_runtime_root_bytes`.
- The production-build audit fails if the workflow stops using the slim
  runtime staging root.

## Verification Results

Local verification completed on 2026-06-22:

```text
bash -n tools/check-ci-production-builds                              # passed
bash -n tools/wordpress-phpunit-timing-rollup                         # passed
bash -n tools/wordpress-phpunit-timing-rollup-test                    # passed
tools/wordpress-phpunit-timing-rollup-test                            # passed
tools/check-ci-production-builds                                      # passed
ctest --preset prod -R '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$' --output-on-failure
                                                                      # passed, 2/2 in 3.43s
cmake --build --preset format-check-prod                              # passed
git diff --check                                                      # passed
```

A local staged-artifact smoke copied the same runtime paths as the workflow,
validated the staged manifest, excluded WordPress Git objects and logs, and
created `build/wordpress-phpunit-runtime-smoke.tar.gz`. It reported:

```text
runtime_root_bytes=225302563
runtime_tar_bytes=82399922
```

Green CI run `27922096226` on `8309b285c` passed the full workflow. The
WordPress timing rollup reported:

```text
wordpress_setup_total_seconds_sum=64.000
wordpress_artifact_pack_runtime_root_bytes=220768494
wordpress_artifact_pack_runtime_tar_bytes=82158143
wordpress_artifact_extract_runtime_tar_bytes_sum=2546902433
wordpress_artifact_download_seconds=80.000
wordpress_artifact_extract_seconds=68.000
wordpress_estimated_workflow_critical_path_seconds=132.000
```

The previous green run `27921463527` reported runtime tar bytes at
`158858467`, extracted runtime tar bytes at `4924612477`, artifact download
at `109.000s`, artifact extract at `94.000s`, and estimated workflow critical
path at `137.000s`.

## Risks And Follow-Up

Keeping only minimal WordPress `.git` metadata assumes PHPUnit tests need a
VCS marker rather than full Git object history. Full WordPress PHPUnit CI is
the compatibility proof for that assumption.

If CI shows staging time outweighs download/extract savings, follow-up work
should avoid the extra staged checkout by streaming the filtered WordPress
snapshot directly into the runtime archive.
