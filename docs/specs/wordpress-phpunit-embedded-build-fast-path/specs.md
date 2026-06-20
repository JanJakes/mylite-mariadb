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
- If `cmake/mariadb-embedded-baseline.cmake` no longer matches the profile
  content signature written by the last successful configure, configure is
  required so profile changes are not hidden without letting checkout/cache
  mtime drift defeat valid CI cache hits.
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

The host wrapper reports Docker image build time and total container phase time,
while the container reports database preparation time separately from PHPUnit.
This keeps CI logs from attributing Docker, setup, or MyLite database creation
variance to the PHP test body.

The harness also splits the coarse build and dependency timers into MariaDB
embedded ensure, MyLite PHP configure/build/wrapper, WordPress Composer install,
PHPUnit tool dependency, and PHPUnit patch buckets. CI sets a build-directory
Composer cache path and restores it with GitHub Actions cache so dependency
download variance is lower and visible when it still occurs.

For full-suite CI, the harness can append a JUnit report path when
`MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`. The CI job uploads that file as an
artifact, giving slow full-suite samples per-test timing evidence without
changing the default local PHPUnit command.

The harness also exposes explicit `MYLITE_WORDPRESS_PHASE` entry points so CI
can split the WordPress job into Docker image build, environment setup,
database preparation, and PHPUnit execution steps while the default local
invocation still runs all phases. Follow-on CI steps set
`MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1` so the visible step timings isolate
prepared-environment work from the test body instead of rebuilding the image
before every phase.

The setup entry point remains available for local callers, but CI now invokes
the narrower `fetch`, `build-php`, and `dependencies` phases separately. That
keeps WordPress checkout time, MariaDB embedded/PHP extension build time, and
Composer/PHPUnit dependency time visible as distinct GitHub Actions steps before
the database preparation and PHPUnit suite steps.

An opt-in `perf-probe` phase reuses the prepared PHP wrapper and database to
measure PHP extension process startup, process startup plus MyLite connect/close,
and steady in-process mysqli loops. That gives a smaller local check for whether
slow PHPUnit samples are dominated by PHP process churn or by SQL execution once
the embedded engine is already open.

The `perf-probe` phase also reports stock PHP process startup, MyLite-extension
wrapper process startup, process plus connect/close, a derived process/connect
delta, in-process mysqli connect/close, and steady SQL throughput. CI runs this
phase with bounded iteration counts as a separate step so per-process startup
and engine-loop timing are visible without scraping the full PHPUnit log.

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
image cache, WordPress fetch, Composer cache, database preparation, and whether
the MariaDB/MyLite build trees are already configured. The harness now emits
phase timings for those setup buckets so slow full-suite samples can be sorted
before blaming ordinary mysqli runtime.

CI dependency setup uses a restored Composer cache under `build/`, but still
runs normal Composer install/require commands inside the same pinned WordPress
and PHPUnit tool directories. The JUnit report is diagnostic output only.

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

After the next ownerless evidence slices through `aa414d91`, a focused
same-machine audit rechecked the pinned WordPress `Tests_DB` filter before
resuming correctness work. Running from the active workspace reported PHPUnit
`00:23.118`, `wordpress_phpunit_shell_real_seconds=47.944`, and
`wordpress_phpunit_seconds=48`; the harness diagnostics showed `/work` on the
workspace filesystem and the database parent on tmpfs, matching the known
source-tree placement artifact. A detached `/tmp` ownerless worktree at
`aa414d91` reported cold-build PHPUnit `00:22.219`,
`wordpress_phpunit_shell_real_seconds=35.500`, and
`wordpress_phpunit_seconds=35`. A detached `/tmp` main worktree at `4760d512`
reported PHPUnit `00:22.186` and `wordpress_phpunit_seconds=35`. Current
focused evidence therefore keeps the WordPress database path at trunk parity
while showing that the branch's visible slowdowns remain attributable to
setup/build, source placement, or the full-suite CI runner band.

After the native snapshot-boundary and transaction snapshot-retry slices
through `926c8952`, the current branch was rechecked because InnoDB source
changes can force a cold embedded rebuild even when ordinary WordPress runtime
stays unchanged. The cold current-head run rebuilt the changed InnoDB archive
and reported `mylite_build_seconds=78`, PHPUnit `00:21.959`,
`wordpress_phpunit_shell_real_seconds=42.375`,
`wordpress_phpunit_shell_user_seconds=20.968`,
`wordpress_phpunit_shell_sys_seconds=15.961`, `wordpress_phpunit_seconds=43`,
and `wordpress_total_seconds=148`. The immediate warm rerun reported
`mariadb_embedded_configure=skipped`, `mylite_build_seconds=5`, PHPUnit
`00:22.336`, `wordpress_phpunit_shell_real_seconds=35.398`,
`wordpress_phpunit_shell_user_seconds=18.730`,
`wordpress_phpunit_shell_sys_seconds=15.828`, `wordpress_phpunit_seconds=36`,
and `wordpress_total_seconds=54`. A same-machine detached `/tmp` main worktree
at `4760d512` reported `mylite_build_seconds=134`, PHPUnit `00:23.195`,
`wordpress_phpunit_seconds=37`, and `wordpress_total_seconds=205` on the older
forced-reconfigure harness path. This keeps the focused WordPress database
runtime close to trunk at the current ownerless head and confirms that the
remaining large wall-time swings come from changed-source rebuilds, main's old
configure path, storage/source placement, or the full-suite CI runner band.

After subsequent ownerless DDL/crash evidence slices through `7310160e`, the
same pinned WordPress ref was rechecked from detached `/tmp` worktrees with the
database also on host `/tmp`. The ownerless current-head probe had a cold build
cache and reported `mylite_build_seconds=350`, PHPUnit `00:21.718`,
`wordpress_phpunit_shell_real_seconds=34.224`,
`wordpress_phpunit_shell_user_seconds=18.180`,
`wordpress_phpunit_shell_sys_seconds=15.107`, `wordpress_phpunit_seconds=35`,
and `wordpress_total_seconds=410`. The detached main worktree at `4760d512`
reported `mylite_build_seconds=104`, PHPUnit `00:22.386`,
`wordpress_phpunit_seconds=34`, and `wordpress_total_seconds=157` on main's
older forced-reconfigure harness path. Current evidence still shows focused
PHPUnit runtime parity; the branch's larger total in this probe was the cold
embedded build, not a slower WordPress database test body.

After additional ownerless column IF EXISTS crash slices through `f8c0ede7`,
the same pinned WordPress ref was rechecked again from a detached `/tmp`
ownerless worktree and a detached `/tmp` main worktree with both databases on
host `/tmp`. The cold ownerless run rebuilt the detached embedded archive and
reported `mariadb_embedded_configure=required`, `mylite_build_seconds=366`,
PHPUnit `00:23.061`, `wordpress_phpunit_shell_real_seconds=34.355`,
`wordpress_phpunit_shell_user_seconds=17.431`,
`wordpress_phpunit_shell_sys_seconds=16.140`, `wordpress_phpunit_seconds=34`,
and `wordpress_total_seconds=422`. The immediate warm ownerless rerun skipped
MariaDB configure, reported `mylite_build_seconds=5`, PHPUnit `00:21.828`,
`wordpress_phpunit_shell_real_seconds=34.840`,
`wordpress_phpunit_shell_user_seconds=18.136`,
`wordpress_phpunit_shell_sys_seconds=16.131`, `wordpress_phpunit_seconds=35`,
and `wordpress_total_seconds=57`. The pinned main worktree at `4760d512`
reported `mylite_build_seconds=122`, PHPUnit `00:21.617`,
`wordpress_phpunit_seconds=33`, and `wordpress_total_seconds=179` on the older
forced-reconfigure harness path. Current focused evidence remains at trunk
parity for the WordPress database runtime; visible wall-clock differences are
still explained by cold embedded rebuilds or harness setup rather than a
slower ordinary mysqli SQL path.

After `b019e624`, the only changes since the previous documented parity check
were ownerless SQL tests and docs. A 2026-06-07 focused recheck still used
detached `/tmp` worktrees and the pinned WordPress ref with both databases on
host `/tmp`. Current ownerless head reported a cold run with
`mariadb_embedded_configure=required`, `mylite_build_seconds=363`, PHPUnit
`00:20.772`, `wordpress_phpunit_shell_real_seconds=31.944`,
`wordpress_phpunit_shell_user_seconds=16.246`,
`wordpress_phpunit_shell_sys_seconds=14.827`, `wordpress_phpunit_seconds=32`,
and `wordpress_total_seconds=422`. The immediate warm ownerless rerun skipped
MariaDB configure, reported `mylite_build_seconds=5`, PHPUnit `00:20.846`,
`wordpress_phpunit_shell_real_seconds=31.594`,
`wordpress_phpunit_shell_user_seconds=16.451`,
`wordpress_phpunit_shell_sys_seconds=14.326`, `wordpress_phpunit_seconds=32`,
and `wordpress_total_seconds=54`. The pinned main worktree at `4760d512`
reported a no-compile but forced-reconfigure run with `mylite_build_seconds=124`,
PHPUnit `00:21.574`, `wordpress_phpunit_seconds=33`, and
`wordpress_total_seconds=177` on the older `all` harness path. Current focused
evidence therefore remains at trunk parity for ordinary WordPress mysqli
runtime; the branch's shorter warm wrapper time is the expected `ensure` fast
path, while slow-looking full jobs still need to be judged against PHPUnit's
timer, build-cache state, source/storage placement, and runner band.

After `b9947b0a`, the current ownerless workspace was checked again with the
same pinned WordPress ref and host `/tmp` database placement. The ownerless run
rebuilt the touched MyLite library and reported `mariadb_embedded_configure=skipped`,
`mylite_build_seconds=16`, PHPUnit `00:21.540`,
`wordpress_phpunit_shell_real_seconds=36.842`,
`wordpress_phpunit_shell_user_seconds=17.614`,
`wordpress_phpunit_shell_sys_seconds=15.890`, `wordpress_phpunit_seconds=37`,
and `wordpress_total_seconds=67`. The same-machine detached main worktree at
`4760d512` reported `mylite_build_seconds=100`, PHPUnit `00:22.394`,
`wordpress_phpunit_seconds=36`, and `wordpress_total_seconds=155` on main's
older forced-configure harness path. This keeps focused PHPUnit runtime at
trunk parity; the remaining large wrapper differences are build/cache behavior,
not a slower ordinary mysqli SQL path.

After `406afa5b`, the phase-timing harness slice re-ran the same pinned
`Tests_DB` probe from the ownerless workspace with the database on host `/tmp`.
The run reported `wordpress_docker_build_seconds=2`,
`mariadb_embedded_configure=skipped`, `mylite_build_seconds=5`,
`wordpress_dependency_seconds=7`, `wordpress_prepare_db_seconds=1`, PHPUnit
`00:20.635`, `wordpress_phpunit_shell_real_seconds=31.555`,
`wordpress_phpunit_shell_user_seconds=16.170`,
`wordpress_phpunit_shell_sys_seconds=14.546`, `wordpress_phpunit_seconds=32`,
`wordpress_container_seconds=49`, and `wordpress_total_seconds=51`. That keeps
the focused database runtime close to trunk while proving the CI log now
separates Docker/setup/database-preparation time from PHPUnit runtime.

After `a0a33a46`, the CI observability/cache slice re-ran the same pinned
`Tests_DB` probe with `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1`. The run reported
`wordpress_docker_build_seconds=3`, `mariadb_embedded_configure=skipped`,
`mylite_mariadb_embedded_seconds=1`, `mylite_php_configure_seconds=0`,
`mylite_php_build_seconds=4`, `mylite_php_wrapper_seconds=0`,
`mylite_build_seconds=5`, `wordpress_composer_install_seconds=7`,
`wordpress_phpunit_tool_dependency_seconds=0`,
`wordpress_phpunit_patch_seconds=0`, `wordpress_dependency_seconds=7`,
`wordpress_prepare_db_seconds=1`, PHPUnit `00:21.206`,
`wordpress_phpunit_shell_real_seconds=32.501`,
`wordpress_phpunit_shell_user_seconds=16.680`,
`wordpress_phpunit_shell_sys_seconds=14.969`, `wordpress_phpunit_seconds=32`,
`wordpress_container_seconds=49`, and `wordpress_total_seconds=52`. The
generated `build/wordpress-phpunit-reports/phpunit-junit.xml` was present,
recorded 651 tests with 3 skips, and included class/testcase timing such as a
separate `Tests_DB` suite time. This keeps focused runtime at parity while
giving full-suite CI enough per-test evidence to distinguish runner-band
swings from a MyLite database-path regression.

After `edb73d1c`, the current ownerless workspace was checked again against
the same pinned WordPress ref with both database directories on host `/tmp`.
The host was moderately loaded, with 18 cores and load average around 14-15, so
the comparison used back-to-back runs and compared PHPUnit's own timer plus the
harness `wordpress_phpunit_seconds` value instead of total wall time alone. The
ownerless run reported `wordpress_docker_build_seconds=4`,
`mariadb_embedded_configure=skipped`, `mylite_mariadb_embedded_seconds=3`,
`mylite_build_seconds=10`, `wordpress_dependency_seconds=5`,
`wordpress_prepare_db_seconds=1`, PHPUnit `00:23.705`,
`wordpress_phpunit_shell_real_seconds=41.571`,
`wordpress_phpunit_shell_user_seconds=19.973`,
`wordpress_phpunit_shell_sys_seconds=16.845`, `wordpress_phpunit_seconds=42`,
and `wordpress_total_seconds=70`. The same-machine main worktree at
`4760d512` reported a no-compile but forced-reconfigure run with
`mylite_build_seconds=125`, PHPUnit `00:30.679`,
`wordpress_phpunit_seconds=50`, and `wordpress_total_seconds=198` on main's
older `all` harness path. This current-head focused probe does not reproduce a
WordPress database-runtime regression; the visible total-time difference is
again dominated by setup/build behavior and the branch's warmed `ensure` fast
path.

After the ownerless SQL shard-balancing and embedded CTest scheduling slices
through `263eca20`, the same pinned `Tests_DB` comparison was repeated from a
detached `/tmp` ownerless worktree and the detached `/tmp` main worktree at
`4760d512`, with both database directories on host `/tmp`. The ownerless cold
run rebuilt the embedded archive and reported `wordpress_docker_build_seconds=5`,
`mariadb_embedded_configure=required`, `mylite_mariadb_embedded_seconds=403`,
`mylite_build_seconds=418`, `wordpress_dependency_seconds=8`,
`wordpress_prepare_db_seconds=1`, PHPUnit `00:23.933`,
`wordpress_phpunit_shell_real_seconds=35.484`,
`wordpress_phpunit_shell_user_seconds=17.936`,
`wordpress_phpunit_shell_sys_seconds=16.008`, `wordpress_phpunit_seconds=35`,
`wordpress_container_seconds=478`, and `wordpress_total_seconds=483`. The
immediate warm rerun skipped MariaDB configure and reported
`wordpress_docker_build_seconds=5`, `mariadb_embedded_configure=skipped`,
`mylite_mariadb_embedded_seconds=0`, `mylite_build_seconds=5`,
`wordpress_dependency_seconds=1`, `wordpress_prepare_db_seconds=1`, PHPUnit
`00:24.164`, `wordpress_phpunit_shell_real_seconds=36.581`,
`wordpress_phpunit_shell_user_seconds=19.619`,
`wordpress_phpunit_shell_sys_seconds=16.155`, `wordpress_phpunit_seconds=37`,
`wordpress_container_seconds=55`, and `wordpress_total_seconds=60`. The
same-machine main worktree reported `mylite_build_seconds=128`, PHPUnit
`00:23.448`, `wordpress_phpunit_seconds=37`, and `wordpress_total_seconds=189`
on main's older forced-reconfigure harness path. The current branch remains at
trunk parity for focused PHPUnit execution; slow-looking totals are still build
and setup state unless PHPUnit's own timer and `wordpress_phpunit_seconds`
move outside the documented band together.

After the later ownerless pressure-policy and schema-drop replay slices through
`d7b6e00a`, the current branch was rechecked from a detached `/tmp` worktree
against the same detached `/tmp` main worktree at `4760d512`, using the pinned
CI WordPress ref and host `/tmp` database placement. The host had 18 cores and
a load average near 18 during the probe, so the comparison again used PHPUnit's
own timer and `wordpress_phpunit_seconds` instead of total wrapper time alone.
The ownerless cold run rebuilt the embedded archive and reported PHPUnit
`00:23.284`, `wordpress_phpunit_shell_real_seconds=35.002`,
`wordpress_phpunit_shell_user_seconds=18.202`,
`wordpress_phpunit_shell_sys_seconds=15.943`, `wordpress_phpunit_seconds=35`,
and `wordpress_total_seconds=515`; the immediate warm rerun skipped MariaDB
configure and reported `mylite_build_seconds=5`, PHPUnit `00:22.940`,
`wordpress_phpunit_shell_real_seconds=34.066`,
`wordpress_phpunit_shell_user_seconds=17.229`,
`wordpress_phpunit_shell_sys_seconds=15.858`, `wordpress_phpunit_seconds=34`,
and `wordpress_total_seconds=54`. The same-machine main worktree reported
`mylite_build_seconds=119`, PHPUnit `00:23.204`,
`wordpress_phpunit_seconds=35`, and `wordpress_total_seconds=180` on main's
older forced-reconfigure harness path. This keeps the focused WordPress
database runtime at trunk parity; the current branch's slow-looking cold total
is still native build/setup work, not a slower PHPUnit body. The full
WordPress CI job for `d7b6e00a` also completed successfully in the documented
slow runner band: `mylite_build_seconds=379`, PHPUnit `28:49.075`,
`wordpress_phpunit_shell_real_seconds=1734.217`,
`wordpress_phpunit_shell_user_seconds=617.303`,
`wordpress_phpunit_shell_sys_seconds=1000.762`,
`wordpress_phpunit_seconds=1734`, `wordpress_container_seconds=2120`, and
`wordpress_total_seconds=2148`. That remains close to the earlier pinned main
slow sample at PHPUnit `28:21.227` and `wordpress_phpunit_seconds=1706`, and
to the earlier ownerless slow sample at PHPUnit `29:09.152` and
`wordpress_phpunit_seconds=1754`.

On 2026-06-07, the WordPress harness was split into explicit CI phases. A local
pinned `Tests_DB` phase run showed cached Docker image build `3s`, warmed setup
`17s`, database preparation `2s` inside the container, and PHPUnit `00:24.932`
with `wordpress_phpunit_seconds=43`. The follow-on performance probe showed
that ordinary mysqli runtime was close to trunk but also identified one
branch-only per-statement cost: ownerless prepared-statement support retained a
copy of every SQL string even for non-ownerless statements. The implementation
now stores that SQL text only for ownerless statements.

After that cleanup, the focused pinned `Tests_DB` phase reported PHPUnit
`00:22.970`, `wordpress_phpunit_shell_real_seconds=39.221`,
`wordpress_phpunit_shell_user_seconds=19.186`,
`wordpress_phpunit_shell_sys_seconds=16.802`, and
`wordpress_phpunit_seconds=39`. A fresh host-`/tmp` branch `perf-probe` reported
PHP extension process startup `74.811ms`, process plus MyLite connect/close
`542.647ms`, `SELECT 1` `257.03 ops/s`, transactional inserts `405.42 ops/s`,
and primary-key point selects `247.00 ops/s`. The same-machine main
`4760d512` probe reported process startup `94.602ms`, process plus connect/close
`515.826ms`, `SELECT 1` `262.22 ops/s`, transactional inserts `387.10 ops/s`,
and point selects `252.97 ops/s`, keeping both per-process startup and
steady-state engine loops close to trunk.

The phase-performance follow-up splits the former CI setup step into
`fetch`, `build-php`, and `dependencies`, then runs the WordPress mysqli
`perf-probe` before the PHPUnit suite. The probe now distinguishes stock PHP
process startup from process startup with MyLite extensions loaded, repeated
short-lived process-plus-connect cost from in-process connect/close cost, and
steady SQL loop throughput. This gives CI a visible per-process startup signal
and a steady engine signal alongside the full PHPUnit suite timing. Detailed
local verification and timing keys are recorded in
`docs/specs/wordpress-phpunit-phase-perf-probe/specs.md`.

## Test Plan

- Run `bash -n tools/mariadb-embedded-build`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the pinned WordPress `Tests_DB` harness on a warmed tree and confirm it
  reports `mariadb_embedded_configure=skipped` and
  `wordpress_phpunit_shell_real_seconds`.
- Run the pinned WordPress `Tests_DB` harness with
  `MYLITE_WORDPRESS_PHPUNIT_LOG_JUNIT=1` and confirm it writes
  `build/wordpress-phpunit-reports/phpunit-junit.xml`.
- Confirm the WordPress harness reports `wordpress_docker_build_seconds`,
  `wordpress_container_seconds`, and `wordpress_prepare_db_seconds`.
- Confirm the WordPress harness reports the split build and dependency phase
  timings.
- Confirm the WordPress harness reports host/container database paths,
  build/check-out paths, CPU count, and `df -h` output before fetching
  WordPress.
- Confirm the CI workflow restores a `build/wordpress-composer-cache` cache and
  uploads the JUnit timing report when present.
- Confirm the CI workflow invokes the WordPress harness as separate
  `docker-image`, `fetch`, `build-php`, `dependencies`, `prepare-db`,
  `perf-probe`, and `phpunit` phases.
- Run the opt-in WordPress `perf-probe` phase after setup/database preparation
  and confirm it reports stock PHP process startup, MyLite-extension process
  startup, process-plus-connect, in-process connect/close, `SELECT 1`, insert,
  autocommit insert, and point-select timings.
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
- The WordPress harness reports host Docker build, container, and database
  preparation timings separately from PHPUnit runtime.
- The WordPress harness reports split build and dependency timings.
- Full-suite WordPress CI uploads a JUnit timing artifact and uses a persistent
  Composer cache path.
- Full-suite WordPress CI shows separate visible timings for Docker image
  build, WordPress fetch, MyLite PHP extension build, dependency installation,
  database preparation, performance probing, and PHPUnit execution.
- The opt-in WordPress `perf-probe` phase reports per-process startup/connect
  cost, in-process connect/close cost, and extension-load process cost
  separately from steady in-process SQL loop throughput, with transactional and
  autocommit insert rates reported independently.
- The WordPress harness reports storage placement and resource diagnostics
  needed to distinguish database-runtime regressions from setup, filesystem, or
  runner variance.
- Full-suite WordPress CI timing remains close to main, and stale non-main CI
  runs are cancelled by newer pushes on the same ref.
