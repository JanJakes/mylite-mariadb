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
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens WordPress
  mysqli connections with `MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE`, not
  `MYLITE_OPEN_OWNERLESS_RW`, so the WordPress harness measures ordinary
  embedded mysqli runtime. Ownerless lock, page-version, dictionary-refresh,
  and checkpoint machinery must stay gated out of that hot path.

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

The harness also reports the host and container paths that influence timing:
WordPress checkout, PHPUnit tools, MariaDB build tree, PHP-extension build tree,
MyLite database directory, PHPUnit arguments, processor count, and `df -h`
output for the relevant parent directories. Slow full-suite CI samples can then
be interpreted against the actual storage placement and runner resource state
instead of only the total job duration.

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

On 2026-06-05 after ownerless commits through `99af858d`, no
`libmylite`, PHP extension, WordPress harness, or CI workflow files had changed
since the `287b6be4` parity probe. A fresh current-machine pinned `Tests_DB`
check initially showed current ownerless head at PHPUnit `00:27.172` with
`wordpress_phpunit_seconds=80`, while a separate `origin/main` worktree at
`4760d512` showed PHPUnit `00:25.251` with `wordpress_phpunit_seconds=37`.
The ownerless wrapper gap did not reproduce after the warmed branch rerun:
ownerless head reported PHPUnit `00:23.166`,
`wordpress_phpunit_shell_real_seconds=37.119`,
`wordpress_phpunit_shell_user_seconds=19.425`,
`wordpress_phpunit_shell_sys_seconds=16.980`,
`wordpress_phpunit_seconds=37`, and `wordpress_total_seconds=66`.

That keeps the DB-focused WordPress runtime close to main and identifies the
earlier slow-looking ownerless wrapper sample as transient setup/host variance
rather than a reproducible ownerless hot-path regression. The harness now wraps
the PHPUnit command with Bash `time`, so future CI logs include process-level
real/user/sys timing in addition to PHPUnit's own timer and the existing wrapper
seconds.

Later on 2026-06-05, after the test/docs-only ownerless commits through
`f677adba`, a same-machine pinned `Tests_DB` comparison again kept the branch
close to main:

- ownerless tree with the in-progress schema-create test/docs slice: PHPUnit
  `00:21.433`, `wordpress_phpunit_shell_real_seconds=36.494`,
  `wordpress_phpunit_shell_user_seconds=18.233`,
  `wordpress_phpunit_shell_sys_seconds=15.519`,
  `wordpress_phpunit_seconds=36`, and `wordpress_total_seconds=59`.
- main `4760d512`: PHPUnit `00:21.684`, `wordpress_phpunit_seconds=35`, and
  `wordpress_total_seconds=160`, including `mylite_build_seconds=111` from the
  old forced-reconfigure harness path.

The full GitHub Actions WordPress job for `f677adba` also completed green:
`mariadb_embedded_configure=required`, `mylite_build_seconds=383`, PHPUnit
`29:39.418`, `wordpress_phpunit_shell_real_seconds=1784.451`,
`wordpress_phpunit_shell_user_seconds=654.876`,
`wordpress_phpunit_shell_sys_seconds=1009.512`,
`wordpress_phpunit_seconds=1784`, and `wordpress_total_seconds=2207`. Compared
with the documented pinned main full-suite baseline at PHPUnit `28:21.227`,
`wordpress_phpunit_seconds=1706`, and `wordpress_total_seconds=2155`, the
ownerless full-suite runtime was about 4.6% higher and total job wall time was
about 2.4% higher. That is within the observed full-suite runner/cache band and
does not reproduce the earlier multi-x ordinary mysqli regression.

On 2026-06-06, after ownerless view crash-recovery slices through `89d584e1`,
a same-machine pinned `Tests_DB` comparison again kept the branch at parity with
main on host-`/tmp` database storage:

- ownerless head `89d584e1`: `mariadb_embedded_configure=skipped`,
  `mylite_build_seconds=11`, PHPUnit `00:22.984`,
  `wordpress_phpunit_shell_real_seconds=38.493`,
  `wordpress_phpunit_shell_user_seconds=18.773`,
  `wordpress_phpunit_shell_sys_seconds=17.179`,
  `wordpress_phpunit_seconds=39`, and `wordpress_total_seconds=65`.
- main `4760d512`: old harness path with a cold MariaDB embedded build,
  `mylite_build_seconds=317`, PHPUnit `00:23.989`,
  `wordpress_phpunit_seconds=39`, and `wordpress_total_seconds=376`.

This comparison found no ordinary WordPress mysqli runtime regression at the
current ownerless head. The slower-looking CI runs around the same commits were
caused by full-suite PHPUnit duration, cold setup/build cost, branch run
cancellation from rapid pushes, and unrelated CI failures, not by a focused
`Tests_DB` runtime cliff.

Later on 2026-06-06, after ownerless view diagnostic slices through
`a1b24361`, a same-machine pinned `Tests_DB` comparison again kept the branch
close to main while the host was noisy:

- ownerless head `a1b24361`: `mariadb_embedded_configure=skipped`,
  `mylite_build_seconds=6`, PHPUnit `00:19.795`,
  `wordpress_phpunit_shell_real_seconds=33.084`,
  `wordpress_phpunit_shell_user_seconds=16.834`,
  `wordpress_phpunit_shell_sys_seconds=14.046`,
  `wordpress_phpunit_seconds=33`, and `wordpress_total_seconds=50`.
- main `4760d512`: old harness path with a cold MariaDB embedded build,
  `mylite_build_seconds=273`, PHPUnit `00:20.404`,
  `wordpress_phpunit_seconds=31`, and `wordpress_total_seconds=323`.

This spot-check found no current focused WordPress database-runtime regression
from the ownerless branch. The large total-wrapper delta is setup/build cost,
not PHPUnit execution time, and the branch's warmed fast path is intentionally
shorter than main's old forced-reconfigure path.

The full GitHub Actions WordPress job for `a1b24361` also completed
successfully despite a separate `ubuntu-embedded` test failure in the same CI
run: `mariadb_embedded_configure=required`, `mylite_build_seconds=352`,
PHPUnit `17:33.422`, `wordpress_phpunit_shell_real_seconds=1057.440`,
`wordpress_phpunit_shell_user_seconds=542.542`,
`wordpress_phpunit_shell_sys_seconds=393.948`, `wordpress_phpunit_seconds=1057`,
and `wordpress_total_seconds=1449`. That matches the faster documented main
full-suite baseline, which reported PHPUnit `17:31.277` and
`wordpress_phpunit_seconds=1055`, and confirms the current branch does not have
a full-suite WordPress PHPUnit runtime cliff.

Later on 2026-06-06, the full-suite WordPress job around `2120ccd7` and
`8a35123e` reported the slower runner band again: `2120ccd7` completed the
WordPress job with PHPUnit `29:07.630`,
`wordpress_phpunit_shell_real_seconds=1752.643`,
`wordpress_phpunit_shell_sys_seconds=1005.508`,
`wordpress_phpunit_seconds=1753`, and `wordpress_total_seconds=2176`;
`8a35123e` reported PHPUnit `28:54.725`,
`wordpress_phpunit_shell_real_seconds=1739.673`,
`wordpress_phpunit_shell_sys_seconds=1005.858`,
`wordpress_phpunit_seconds=1739`, and `wordpress_total_seconds=2150` while the
overall run failed in `ubuntu-embedded`, not WordPress. After the ownerless
native-hook shutdown reset and page-write refresh hardening, current head
`b38502b1` completed the same full-suite WordPress job green with
`mariadb_embedded_configure=required`, `mylite_build_seconds=359`, PHPUnit
`18:05.762`, `wordpress_phpunit_shell_real_seconds=1089.913`,
`wordpress_phpunit_shell_user_seconds=556.083`,
`wordpress_phpunit_shell_sys_seconds=393.555`, `wordpress_phpunit_seconds=1089`,
and `wordpress_total_seconds=1490`. This latest CI sample is below both the
documented main `28:21.227`/`1706s` baseline and the faster main
`17:31.277`/`1055s` runner band, so the current branch does not show a
sustained full-suite PHPUnit regression.

The same day, a same-machine pinned `Tests_DB` comparison at current head kept
ordinary WordPress database runtime at main parity on host-`/tmp` storage:

- ownerless head `b38502b1`: warmed branch run with
  `mariadb_embedded_configure=skipped`, `mylite_build_seconds=4`, PHPUnit
  `00:22.464`, `wordpress_phpunit_shell_real_seconds=34.004`,
  `wordpress_phpunit_shell_user_seconds=17.939`,
  `wordpress_phpunit_shell_sys_seconds=15.345`, `wordpress_phpunit_seconds=34`,
  and `wordpress_total_seconds=46`.
- main `4760d512`: detached `/tmp` worktree with the old forced-reconfigure
  harness path, `mylite_build_seconds=88`, PHPUnit `00:22.455`,
  `wordpress_phpunit_seconds=34`, and `wordpress_total_seconds=138`.

This current focused probe shows no ordinary mysqli runtime regression. The
branch wrapper is now faster than main on warmed runs because it reuses the
embedded archive and builds only the PHP extension targets WordPress loads.

After later ownerless slices through `c8614604`, a 2026-06-06 performance audit
rechecked the same pinned WordPress ref. Running ownerless from the active
workspace with only the database on `/tmp` reported PHPUnit `00:21.205`,
`wordpress_phpunit_shell_real_seconds=40.701`, and
`wordpress_phpunit_seconds=40`, while main `4760d512` from a detached `/tmp`
worktree reported PHPUnit `00:21.254`, `wordpress_phpunit_seconds=34`, and
`wordpress_total_seconds=177`. That comparison was not storage-equivalent
because the ownerless WordPress/PHPUnit source tree was still on the workspace
filesystem. Repeating ownerless from a detached `/tmp` worktree matched the
main filesystem placement and removed the apparent wrapper gap: the cold
ownerless run reported `mylite_build_seconds=366`, PHPUnit `00:22.085`,
`wordpress_phpunit_shell_real_seconds=33.743`, and
`wordpress_phpunit_seconds=33`; the warm ownerless rerun reported
`mariadb_embedded_configure=skipped`, `mylite_build_seconds=4`, PHPUnit
`00:23.164`, `wordpress_phpunit_shell_real_seconds=34.988`,
`wordpress_phpunit_shell_user_seconds=18.110`,
`wordpress_phpunit_shell_sys_seconds=16.075`, `wordpress_phpunit_seconds=35`,
and `wordpress_total_seconds=64`. Full-suite ownerless CI completed green in
both observed runner bands around the audit. Run `27059012651` at `25697f61`
reported `mylite_build_seconds=360`, PHPUnit `17:47.337`,
`wordpress_phpunit_shell_real_seconds=1071.532`,
`wordpress_phpunit_shell_user_seconds=552.882`,
`wordpress_phpunit_shell_sys_seconds=395.692`, `wordpress_phpunit_seconds=1072`,
and `wordpress_total_seconds=1469`; current-head run `27059744570` at
`c8614604` reported `mylite_build_seconds=377`, PHPUnit `29:09.152`,
`wordpress_phpunit_shell_real_seconds=1754.090`,
`wordpress_phpunit_shell_user_seconds=629.441`,
`wordpress_phpunit_shell_sys_seconds=1002.392`, `wordpress_phpunit_seconds=1754`,
and `wordpress_total_seconds=2179`. The focused and CI evidence therefore does
not show a sustained branch PHPUnit regression; slow-looking samples need to be
interpreted against build-cache state, filesystem placement, Composer/fetch
work, and the GitHub runner band.

After the ownerless sequence-expression guard and foreign-key graph diagnostic
slices through `09b95cc5`, the WordPress-relevant runtime delta was limited to
`mariadb/sql/item_func.cc` sequence function guards plus ownerless SQL
test/docs changes; the PHP adapter, WordPress harness, CI workflow, and
ordinary `database.cc` SQL path were unchanged from the previous parity audit.
A detached `/tmp` worktree at current head, pinned to the CI WordPress ref,
reported a cold `Tests_DB` probe with `mylite_build_seconds=552`, PHPUnit
`00:23.402`, `wordpress_phpunit_shell_real_seconds=36.576`,
`wordpress_phpunit_shell_user_seconds=19.049`,
`wordpress_phpunit_shell_sys_seconds=16.470`, and
`wordpress_phpunit_seconds=36`. The immediate warm rerun skipped MariaDB
configure, reported `mylite_build_seconds=6`, PHPUnit `00:26.387`,
`wordpress_phpunit_shell_real_seconds=40.243`,
`wordpress_phpunit_shell_user_seconds=20.034`,
`wordpress_phpunit_shell_sys_seconds=18.423`, and
`wordpress_phpunit_seconds=41`. A same-machine pinned main worktree at
`4760d512` on the older forced-reconfigure harness path reported
`mylite_build_seconds=175`, PHPUnit `00:31.934`, and
`wordpress_phpunit_seconds=61`. The latest completed full WordPress CI sample
at `27fd2113` reported `mylite_build_seconds=393`, PHPUnit `27:48.011`,
`wordpress_phpunit_shell_real_seconds=1672.964`,
`wordpress_phpunit_shell_user_seconds=582.260`,
`wordpress_phpunit_shell_sys_seconds=955.518`, and
`wordpress_phpunit_seconds=1673`, which remains in the documented main
full-suite runner band rather than showing a current branch PHPUnit cliff.

After the FK row-step crash slice and ownerless SQL shard split through
`49b5c0aa`, another 2026-06-06 audit rechecked the same pinned WordPress ref
before pursuing CI-only fixes. Running ownerless from the active workspace with
the database on host `/tmp` reported PHPUnit `00:24.517`,
`wordpress_phpunit_shell_real_seconds=56.235`,
`wordpress_phpunit_shell_user_seconds=23.734`,
`wordpress_phpunit_shell_sys_seconds=19.807`, and
`wordpress_phpunit_seconds=56`; that repeated the known workspace-source-tree
placement artifact. A detached `/tmp` ownerless worktree then reported a cold
run with `mylite_build_seconds=433`, PHPUnit `00:24.377`,
`wordpress_phpunit_shell_real_seconds=37.522`,
`wordpress_phpunit_shell_user_seconds=19.570`,
`wordpress_phpunit_shell_sys_seconds=17.079`, and
`wordpress_phpunit_seconds=38`. The immediate warm rerun skipped MariaDB
configure, reported `mylite_build_seconds=5`, PHPUnit `00:22.996`,
`wordpress_phpunit_shell_real_seconds=35.696`,
`wordpress_phpunit_shell_user_seconds=18.291`,
`wordpress_phpunit_shell_sys_seconds=16.284`, and
`wordpress_phpunit_seconds=36`. A same-machine detached main worktree at
`4760d512` reported `mylite_build_seconds=157`, PHPUnit `00:21.515`,
`wordpress_phpunit_seconds=37`, and `wordpress_total_seconds=241` on the older
forced-reconfigure harness path. Current evidence therefore keeps the focused
WordPress database runtime close to main while showing that branch warmed setup
is much shorter than main's old harness path; slow-looking samples remain
setup/storage/runner artifacts unless PHPUnit's own timer and the wrapper
`wordpress_phpunit_seconds` move together outside this band.

After adding harness resource diagnostics, a pinned `Tests_DB` run from the
active workspace reported the new path/resource lines before fetching
WordPress: host database directory `/tmp/mylite-wordpress-tests-1531901641.mylite`,
container database directory
`/mylite-wordpress-db/mylite-wordpress-tests-1531901641.mylite`,
`wordpress_nproc=18`, `/work` at 88% used, and the database parent on tmpfs.
The warmed run skipped MariaDB configure, reported `mylite_build_seconds=6`,
`wordpress_dependency_seconds=17`, PHPUnit `00:21.877`,
`wordpress_phpunit_shell_real_seconds=42.182`, and
`wordpress_phpunit_seconds=42`. This keeps PHPUnit's own focused DB timer in
the documented main range while exposing the source/build storage placement
needed to interpret wrapper-time drift.

The first full WordPress CI run with harness resource diagnostics,
`27067030178` at ownerless head `74434299`, completed successfully in the slow
full-suite band. The log reported `wordpress_nproc=4`, `/work` and
`/mylite-wordpress-db` both on `/dev/root` at 40% used, host database directory
`/tmp/mylite-wordpress-tests-3974124642.mylite`, and container database
directory `/mylite-wordpress-db/mylite-wordpress-tests-3974124642.mylite`.
The cold run required MariaDB configure, reported `mylite_build_seconds=374`,
`wordpress_dependency_seconds=3`, PHPUnit `28:55.451`,
`wordpress_phpunit_shell_real_seconds=1740.490`,
`wordpress_phpunit_shell_user_seconds=622.126`,
`wordpress_phpunit_shell_sys_seconds=1002.014`,
`wordpress_phpunit_seconds=1740`, and `wordpress_total_seconds=2151`.
Compared with the documented main slow-band baseline at PHPUnit `28:21.227`,
`wordpress_phpunit_seconds=1706`, and `wordpress_total_seconds=2155`, this
keeps the full-suite branch runtime close to main while confirming that the
large visible swing is the GitHub runner/full-suite band.

## Test Plan

- Run `bash -n tools/mariadb-embedded-build`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the pinned WordPress `Tests_DB` harness on a warmed tree and confirm it
  reports `mariadb_embedded_configure=skipped` and
  `wordpress_phpunit_shell_real_seconds`.
- Confirm the WordPress harness reports host/container database paths,
  build/check-out paths, CPU count, and `df -h` output before fetching
  WordPress.
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
- The WordPress harness reports shell real/user/sys timing for the PHPUnit
  process.
- The WordPress harness reports storage placement and resource diagnostics
  needed to distinguish database-runtime regressions from setup, filesystem, or
  runner variance.
- Full-suite WordPress CI timing remains close to main, and stale non-main CI
  runs are cancelled by newer pushes on the same ref.
