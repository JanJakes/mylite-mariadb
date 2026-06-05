# WordPress PHPUnit Embedded Build Fast Path

## Problem

The ownerless branch's ordinary WordPress database runtime is now close to the
pinned main baseline, but CI and local wrapper wall time can still look much
slower because the WordPress harness calls `tools/mariadb-embedded-build all` on
every run. That command intentionally runs `cmake --fresh`, so even a warm
MariaDB embedded build tree pays the full configure probe before Ninja reports
that `libmariadbd.a` has no work.

Current pinned `Tests_DB` measurements on the same host class show the split:

- ownerless head `c2c09656`: PHPUnit `00:21.746`,
  `wordpress_phpunit_seconds=33`, `mylite_build_seconds=118`,
  `wordpress_total_seconds=164`.
- pinned main `4760d512`: PHPUnit `00:21.004`,
  `wordpress_phpunit_seconds=33`, `mylite_build_seconds=120`,
  `wordpress_total_seconds=168`.

The runtime delta is therefore small; the visible slowness is mostly repeated
MariaDB configure work and cache state.

## Source Findings

- `tools/wordpress-phpunit-mysqli-mylite` invokes
  `BUILD_DIR=... tools/mariadb-embedded-build all` before configuring the PHP
  extension build.
- `tools/mariadb-embedded-build all` runs `configure`, `build`, and `measure`.
- `configure` uses `cmake --fresh`, so CMake intentionally discards the existing
  cache and repeats the MariaDB feature-detection probes on every call.
- `build` already invokes the configured build-system target and keeps the
  archive strip step idempotent through the `.mylite-stripped` marker.
- MyLite CMake targets also run
  `cmake/check-mariadb-embedded-freshness.cmake`, which fails if the embedded
  archive is older than MariaDB sources or
  `cmake/mariadb-embedded-baseline.cmake`.

## Design

Add `tools/mariadb-embedded-build ensure` for integration harnesses:

- If explicit CMake arguments are supplied, configure is required.
- If the MariaDB build tree has no `CMakeCache.txt`, configure is required.
- If the cache was created for a different absolute build directory, configure
  is required so host and Docker path reuse does not fail in CMake.
- If `cmake/mariadb-embedded-baseline.cmake` is newer than `CMakeCache.txt`,
  configure is required so profile changes are not hidden.
- Otherwise, skip configure and run the normal `build` plus `measure` steps.

Switch the WordPress PHPUnit harness from `all` to `ensure`. Cold runs still
configure exactly as before, while warmed runs keep the existing build tree and
let Ninja decide whether `libmariadbd.a` needs work.

## Compatibility Impact

No SQL, PHP API, mysqli, or runtime behavior changes. The harness still builds
and loads the same PHP modules and runs the same PHPUnit command.

## Directory And Lifecycle Impact

No MyLite database directory behavior changes. The existing host-temp database
default remains unchanged.

## Build And Performance Impact

Warm WordPress harness runs avoid repeated MariaDB configure probes. The
remaining build phase still checks/builds `libmariadbd.a`, strips only when
needed, reports archive size evidence, configures the MyLite PHP build, and
builds the targeted PHP module list.

Runtime comparisons should continue to use PHPUnit's elapsed time and
`wordpress_phpunit_seconds`. Total wrapper time is still affected by Docker
image cache, WordPress fetch, Composer cache, and whether the MariaDB/MyLite
build trees are already configured.

After switching the harness to `ensure`, the same pinned ownerless `Tests_DB`
run reported `mariadb_embedded_configure=skipped`, `mylite_build_seconds=4`,
PHPUnit `00:21.420`, `wordpress_phpunit_seconds=33`, and
`wordpress_total_seconds=54`.

Full-suite CI remains long because the workflow runs the complete pinned
WordPress PHPUnit suite, not only `Tests_DB`. The latest completed main
baseline `4760d512` reported `mylite_build_seconds=411`, PHPUnit
`Time: 28:21.227`, `wordpress_phpunit_seconds=1706`, and
`wordpress_total_seconds=2155`. Ownerless `5012e9b4` reported
`mariadb_embedded_configure=required`, `mylite_build_seconds=380`, PHPUnit
`Time: 29:19.250`, `wordpress_phpunit_seconds=1764`, and
`wordpress_total_seconds=2181`. That puts the full-suite PHP runtime at about
3.4% above the pinned main baseline and the total wrapper time at about 1.2%
above main for cold CI jobs.

The CI workflow now uses branch-scoped concurrency cancellation for non-main
refs so obsolete pushes do not leave several full WordPress jobs running after a
newer commit supersedes them.

On 2026-06-04, a current-machine pinned `Tests_DB` comparison used the CI
WordPress ref `6ddfc9d9b532c6e95c1266165149815895e2eb56` on comparable host
`/tmp` storage:

- ownerless head `4cb3b1cd`: `mariadb_embedded_configure=skipped`,
  `mylite_build_seconds=5`, PHPUnit `00:22.897`,
  `wordpress_phpunit_seconds=36`, and `wordpress_total_seconds=58`.
- main `4760d512`: old harness path with `mylite_build_seconds=151`, PHPUnit
  `00:22.507`, `wordpress_phpunit_seconds=37`, and
  `wordpress_total_seconds=214`.

This spot-check keeps the relevant PHPUnit runtime at parity with main. The
large wrapper-time difference is expected: main still forces the old broad
`tools/mariadb-embedded-build all` path, while the branch reuses the warmed
embedded build and builds only the PHP extension targets loaded by WordPress.

On 2026-06-05, after the follow-on ownerless foreign-key and trigger crash
slices, the same pinned `Tests_DB` probe remained at parity on the same
machine and comparable host-`/tmp` storage:

- ownerless head `96f02362`: `mariadb_embedded_configure=skipped`,
  `mylite_build_seconds=82`, PHPUnit `00:20.347`,
  `wordpress_phpunit_seconds=35`, and `wordpress_total_seconds=144`.
- main `4760d512`: old harness path with `mylite_build_seconds=90`, PHPUnit
  `00:20.685`, `wordpress_phpunit_seconds=34`, and
  `wordpress_total_seconds=144`.

Recent full-suite CI showed the same shape rather than a sustained ownerless
PHPUnit regression: main runs reported PHPUnit `17:31.277`/`1055s` and
`28:21.227`/`1706s`, while ownerless runs reported
`19:09.291`/`1154s` and `29:07.597`/`1753s`. The full-suite job has a broad
runner/cache band, so regressions should be judged against both PHPUnit's
`Time:` line and `wordpress_phpunit_seconds`, not only total workflow wall
time or cancelled stale branch runs.

On 2026-06-05 after the ownerless transaction page-LSN coverage slice, the
current host was heavily loaded (load average about 19 on 18 cores, with an
unrelated headless Chromium GPU process consuming roughly six CPUs). Under that
load, pinned `Tests_DB` still stayed close to main in PHPUnit's own runtime:

- ownerless head `287b6be4`: `mariadb_embedded_configure=skipped`,
  `mylite_build_seconds=9`, PHPUnit `00:28.037`,
  `wordpress_phpunit_seconds=54`, and `wordpress_total_seconds=87`.
- main `4760d512`: old harness path with a no-op Ninja build but forced
  MariaDB reconfigure, `mylite_build_seconds=162`, PHPUnit `00:25.941`,
  `wordpress_phpunit_seconds=39`, and `wordpress_total_seconds=234`.

The wrapper-phase gap was not reproduced when build, fetch, and Composer setup
were stripped away and both trees ran only the database preparation plus
PHPUnit command against host-`/tmp` database directories:

- main `4760d512`: PHPUnit `00:24.500`, `just_phpunit_seconds=37`.
- ownerless head `287b6be4`: PHPUnit `00:24.289`,
  `just_phpunit_seconds=38`.

This confirms the branch remains at parity for the focused WordPress database
runtime. The slow-looking CI and local wrapper runs are dominated by old main
harness reconfiguration, cache/fetch/dependency phases, and transient host
load, not by a remaining ordinary ownerless SQL hot-path regression.

## Test Plan

- Run `bash -n tools/mariadb-embedded-build`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the pinned WordPress `Tests_DB` harness on a warmed tree and confirm it
  reports `mariadb_embedded_configure=skipped`.
- Confirm the build phase no longer repeats MariaDB configure on a warmed tree.
- Confirm the CI workflow has a branch-scoped concurrency group with main runs
  excluded from automatic cancellation.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- `tools/mariadb-embedded-build ensure` falls back to full configure for
  missing, stale, explicitly changed, or different-path configuration inputs.
- Warmed `ensure` runs skip configure and still run build plus measure.
- The WordPress harness uses `ensure`.
- Pinned WordPress `Tests_DB` remains close to main in PHPUnit elapsed time and
  `wordpress_phpunit_seconds`.
- Full-suite WordPress CI timing remains close to main, and stale non-main CI
  runs are cancelled by newer pushes on the same ref.
