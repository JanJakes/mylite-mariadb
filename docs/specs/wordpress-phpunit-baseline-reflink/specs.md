# WordPress PHPUnit Baseline Reflink

## Problem

The factory-heavy process-isolated WordPress PHPUnit shard needs a clean
database directory before each child. Prior focused skip-install-only coverage
showed the `Tests_Admin_ExportWp` and `Tests_Sitemaps_Sitemaps` filter can leak
invalid WordPress factory object values into later children, so removing the
per-child baseline restore is not currently correct.

The remaining safe performance target is the copy mechanism itself. The harness
created and restored the prepared baseline with plain `cp -a`, which forces a
full directory copy even on filesystems that can clone file extents.

## Source Findings

- `tools/wordpress-phpunit-mysqli-mylite` prepares a reusable WordPress MyLite
  baseline directory during `MYLITE_WORDPRESS_PHASE=prepare-db` when
  `MYLITE_WORDPRESS_PHPUNIT_SKIP_INSTALL=1`.
- The same harness restores the baseline before each PHPUnit phase that uses
  parent skip-install and, for the factory-heavy process-isolated shard, inside
  the patched `DefaultPhpProcess::runProcess()` before launching each child.
- The current CI workflow keeps that shard on
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_RESTORE_BASELINE=1` and child
  `WP_TESTS_SKIP_INSTALL=1` because direct child skip-install is unsafe for the
  covered factory-heavy tests.

## Design

Use GNU `cp -a --reflink=auto --` for the three harness-owned database tree
copies:

- prepared database to baseline during `prepare-db`;
- baseline to active database before each parent PHPUnit phase;
- baseline to active database before each baseline-restored child process.

`--reflink=auto` preserves the existing recursive archive copy semantics and
falls back to a normal copy when the runner filesystem cannot clone extents.
No WordPress test selection, MyLite database contents, embedded lifecycle, or
ownerless coordination behavior changes.

`tools/check-ci-production-builds` now also checks the WordPress harness for the
reflink-auto copy marker so CI timing changes do not silently return to plain
full-copy restores.

## Compatibility Impact

This is a harness performance change only. It does not alter MyLite SQL,
storage-engine, public C API, PHP mysqli behavior, database-directory layout, or
WordPress compatibility expectations.

## Directory And Lifecycle Impact

The source and destination directories are unchanged. The restored active
WordPress MyLite database directory remains separate from the prepared baseline
directory, and the existing parent/child handle-close ordering is unchanged.

## Test Plan

- Run shell syntax checks for the WordPress harness and production CI audit.
- Run `tools/check-ci-production-builds`.
- Re-run the WordPress dependency patch phase so the warmed PHPUnit vendor tree
  receives the updated child baseline restore command.
- Run a focused baseline-restored process-isolated WordPress shard with
  child timing enabled and confirm the same factory-heavy filter passes with
  baseline restore count equal to child count.
- Run `ctest --preset prod -R
  'tools\.ci-production-builds|tools\.wordpress-phpunit-timing-rollup'`.

## Acceptance Criteria

- Baseline-restored child tests still pass.
- Child baseline restore timing remains reported.
- Copy commands use `--reflink=auto` for parent and child baseline restores.
- Existing production CI build/timing guards pass.

## Verification

Against the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, the focused baseline-restored
factory-heavy shard passed `13` tests with `37` assertions. The run reported
`13` child processes, `13` child baseline restores,
`2.905252s` total child baseline-restore time, and `223.481 ms` average restore
time per child on the local tmpfs-backed database mount. A local default-trunk
rerun remained incompatible with this filter and is not used as acceptance
evidence for the CI-pinned shard.

## Risks

On filesystems without copy-on-write clone support, this is expected to be
neutral rather than faster. The option is deliberately `auto` so unsupported
filesystems keep the previous full-copy behavior.
