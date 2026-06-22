# WordPress Runtime Source Snapshot Slimming

## Problem

Commit `8309b285c` cut the WordPress PHPUnit runtime tarball from
`158858467` bytes to `82158143` bytes in green CI run `27922096226`, but the
same run still reported:

- `wordpress_artifact_download_seconds=80`;
- `wordpress_artifact_extract_seconds=68`;
- `wordpress_artifact_extract_runtime_tar_bytes_sum=2546902433`;
- `wordpress_artifact_pack_runtime_root_bytes=220768494`.

The first slim artifact removed full build trees and WordPress Git object
history. The staged WordPress checkout still carries non-PHPUnit test trees
and root Composer development dependencies that the CI PHPUnit runtime does
not use.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `phpunit.xml.dist` runs `tests/phpunit/includes/bootstrap.php` and discovers
  tests under `tests/phpunit/tests`.
- The WordPress PHPUnit bootstrap loads Yoast PHPUnit polyfills from
  `vendor/yoast/phpunit-polyfills/phpunitpolyfills-autoload.php`.
- PHPUnit tests read several root files through `dirname( ABSPATH )`, including
  `SECURITY.md`, `package.json`, `package-lock.json`, and `composer.json`, so
  the runtime snapshot should keep root files rather than hand-picking only
  the currently known reads.
- Local inspection showed non-PHPUnit test trees under `tests/e2e`,
  `tests/gutenberg`, `tests/performance`, `tests/phpstan`, `tests/qunit`, and
  `tests/visual-regression`. The root `vendor/` tree is about `54 MiB`, while
  the PHPUnit-required Yoast polyfills plus Composer autoload metadata are
  under `1 MiB`.

## Design

Keep the explicit runtime staging root from the prior slice, but refine the
WordPress checkout copy:

- continue to exclude `.git/objects`, `.git/logs`, and the root
  `.phpunit.result.cache`;
- exclude non-PHPUnit test trees:
  `tests/e2e`, `tests/gutenberg`, `tests/performance`, `tests/phpstan`,
  `tests/qunit`, and `tests/visual-regression`;
- exclude the root `vendor/` tree during the tar stream;
- copy back only `vendor/autoload.php`, `vendor/composer`, and
  `vendor/yoast/phpunit-polyfills`.

The source snapshot still keeps `src`, `tests/phpunit`, root metadata/config
files, minimal `.git` marker metadata, and the existing manifest-hashed runtime
artifacts. The manifest still verifies the same runtime files as before,
including `vendor/bin/phpunit` from the separate PHPUnit tools directory and
`wp-tests-config.php` from the WordPress checkout.

## Non-Goals

- Changing WordPress PHPUnit filters, shard grouping, test selection, or
  process-isolated settings.
- Changing SQL behavior, mysqli behavior, public C API behavior, native
  storage, directory lifecycle, ownerless locking, redo, checkpoint, or
  recovery behavior.
- Removing Docker from the WordPress PHPUnit job.
- Removing `src`, `tests/phpunit`, root package metadata, or PHPUnit
  polyfills from the runtime snapshot.

## Compatibility Impact

No application compatibility behavior changes. This only changes the CI
transport snapshot used by WordPress PHPUnit shard jobs after the production
setup job has already prepared sources, dependencies, runtime artifacts, and
the database baseline.

## Directory And Lifecycle Impact

No durable MyLite directory layout changes. Shards still restore independent
external MyLite database directories from the verified baseline artifact before
running PHPUnit.

## Native Storage Impact

No native storage format, redo, checkpoint, recovery, locking, or ownerless
page-version behavior changes.

## Build, Size, And Performance Impact

The expected effect is a smaller staged root and runtime tarball, reducing
per-shard artifact download and extract work. Setup staging should also avoid
copying tens of MiB of dev-only vendor files and non-PHPUnit test data.

CI timing remains the performance authority because artifact compression,
download, and extraction vary with GitHub runner conditions.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run a local staged-artifact smoke that validates the manifest and asserts
  omitted/retained WordPress paths.
- Run focused production CTest coverage for the CI production-build audit.
- Run the production format check.
- Run `git diff --check`.
- Let the pushed CI run provide full WordPress PHPUnit compatibility and
  timing evidence for the reduced source snapshot.

## Acceptance Criteria

- The setup job excludes non-PHPUnit test trees and root `vendor/` while
  staging `build/wordpress-develop`.
- The staged WordPress snapshot retains `src`, `tests/phpunit`, root files,
  minimal `.git` marker metadata, Composer autoload metadata, and Yoast
  PHPUnit polyfills.
- The staged manifest check passes before upload.
- The production-build audit fails if the workflow stops enforcing the reduced
  source snapshot.
- Full WordPress PHPUnit CI passes with the reduced snapshot.

## Verification Results

Local verification completed on 2026-06-22:

```text
bash -n tools/check-ci-production-builds                              # passed
tools/check-ci-production-builds                                      # passed
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
                                                                      # passed, 1/1 in 2.93s
cmake --build --preset format-check-prod                              # passed
git diff --check                                                      # passed
```

A local staged-artifact smoke validated the manifest, asserted retained and
omitted WordPress paths, and reported:

```text
runtime_root_bytes=180271906
runtime_tar_bytes=75451546
```

A one-test staged PHPUnit smoke used the reduced source snapshot, the local
runtime Docker image, and the existing prepared WordPress database baseline:

```text
tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_Basic::test_package_json$'
# passed, 1 test / 1 assertion
wordpress_phpunit_reported_seconds=1.266
wordpress_phpunit_shell_real_seconds=13.897
```

CI timing remains pending for the pushed workflow.

## Risks And Follow-Up

The risk is an implicit WordPress PHPUnit dependency on another root Composer
development package or non-PHPUnit test directory. The full WordPress PHPUnit
CI fanout is the compatibility proof for that assumption. If a missing path is
found, add only that path back to the runtime snapshot rather than restoring
the full `vendor/` or `tests/` tree.
