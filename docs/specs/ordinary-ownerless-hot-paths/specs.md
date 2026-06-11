# Ordinary Ownerless Hot Paths

## Problem

Ownerless concurrency adds SQL statement gates, shared transaction/read-view
state, InnoDB lock hooks, redo visibility, and page-version WAL publication.
Those mechanisms are required for `MYLITE_OPEN_OWNERLESS_RW` and shared
read-only opens, but ordinary exclusive embedded opens remain the default path
for single-process application adapters such as WordPress' mysqli integration.

A WordPress `Tests_DB` probe on this branch originally showed the ordinary
mysqli path regressing from the pinned main baseline:

- main `4760d512`: `wordpress_phpunit_seconds=65`, PHPUnit `00:45.258`
- ownerless branch before this slice: `wordpress_phpunit_seconds=157`,
  PHPUnit `02:13.893`
- after the ordinary hot-path slice under high host load:
  `wordpress_phpunit_seconds=66`,
  PHPUnit `00:40.920`; total wrapper time was `376s` because the harness spent
  `262s` in build/setup.
- a final-code confirmation run while the host load average was about 18 and
  an unrelated Chromium GPU process was using roughly seven CPUs completed the
  PHPUnit body with `wordpress_phpunit_seconds=64` and PHPUnit `00:46.007`;
  total wrapper time was `281s`, including `192s` of build/setup and `8s` of
  dependency setup.

Later DDL-focused profiling found a second measurement artifact: the branch
worktree lived under `.paseo`, while the main comparison worktree lived under
host `/tmp`. The WordPress harness default placed the MyLite database under
`/work/build`, so branch and main were syncing DDL files on different host
storage. A small traced DDL loop had nearly identical durable-sync counts
between branch and main, but very different sync latency:

- branch `.paseo` database path: 204 `fdatasync` calls and 3 `fsync` calls;
  `ddl_recovery.log` accounted for 113 `fdatasync` calls and `0.362s`.
- main host-`/tmp` database path: 206 `fdatasync` calls; `ddl_recovery.log`
  accounted for 113 `fdatasync` calls and `0.012s`.

With the WordPress database bind-mounted from the same host-`/tmp` storage for
both builds, the branch is close to main:

- `Tests_DB_dbDelta`: branch PHPUnit `00:03.681`, wrapper `16s`; main PHPUnit
  `00:04`, wrapper `17s`.
- `Tests_DB`: branch PHPUnit `00:22.070`, wrapper `34s`; main PHPUnit
  `00:21.657`, wrapper `33s`.
- Supported harness after the host-temp database default:
  `tools/wordpress-phpunit-mysqli-mylite --filter Tests_DB_dbDelta` completed
  with PHPUnit `00:03.502` and `wordpress_phpunit_seconds=15`.

The slowdown was real for database paths on slow-sync workspace storage, but
not a remaining ordinary ownerless hook leak. The WordPress harness now defaults
the test database to a host-temp directory while preserving
`MYLITE_WORDPRESS_DB_DIR` for explicit placement. A follow-up harness slice also
builds only the `mylite` and `mysqli_mylite` PHP module targets needed by
WordPress, so total wrapper time is less likely to be dominated by unrelated
CMake targets. A later pinned `Tests_DB` comparison at ownerless head
`c2c09656` measured PHPUnit `00:21.746` and
`wordpress_phpunit_seconds=33`, versus pinned main `4760d512` at PHPUnit
`00:21.004` and `wordpress_phpunit_seconds=33`; the remaining wrapper
difference was repeated MariaDB embedded configure work, not ordinary SQL
runtime. The WordPress harness now uses `tools/mariadb-embedded-build ensure`
so warmed runs reuse a compatible MariaDB build cache instead of forcing
`cmake --fresh` before every PHPUnit run.

Full WordPress PHPUnit profiling exercises more process-isolated tests than
the focused `Tests_DB` probe, so ordinary native startup also needs to avoid
ownerless recovery I/O when no ownerless evidence exists. Fresh ordinary
read/write opens now check for the ownerless redo-header backup before touching
the InnoDB redo log for backup validation, and capture the redo startup prefix
only when retained page WAL, a native file-operation checkpoint marker, or a
valid ownerless redo-header backup has already selected the ownerless recovery
bridge.

A later full-suite CI comparison did not show a WordPress runtime regression:
main `4760d512` reported `wordpress_phpunit_seconds=1706` and PHPUnit
`28:21.227`, while ownerless head `fcee3b7c` reported
`wordpress_phpunit_seconds=1725` and PHPUnit `28:40.580` for the same pinned
WordPress SHA; the following generated-column failed-DDL head `9c5aa0ec`
reported `wordpress_phpunit_seconds=1696` and PHPUnit `28:10.946`. The same
audit did find that `libmylite.embedded-open-close` had become a misleading
lifecycle performance signal: main CI reported `2.65s`, while the ownerless
branch reported `35.27s` after ownerless directory and product-hook SQL
coverage were added to the same executable. Those tests are now registered as
separate CTest entries so baseline open/close timing remains visible
independently from ownerless directory and product-hook coverage.

A 2026-06-05 spot-check after later foreign-key and trigger crash slices kept
the pinned WordPress `Tests_DB` runtime at parity: ownerless head `96f02362`
reported PHPUnit `00:20.347` and `wordpress_phpunit_seconds=35`, while main
`4760d512` reported PHPUnit `00:20.685` and
`wordpress_phpunit_seconds=34` on comparable host-`/tmp` storage.

A follow-up 2026-06-05 audit after ownerless transaction page-LSN coverage
checked the same pinned WordPress ref under high host load. A full warmed branch
harness run reported PHPUnit `00:28.037` and
`wordpress_phpunit_seconds=54`, while a warmed main run reported PHPUnit
`00:25.941` and `wordpress_phpunit_seconds=39` but still spent
`mylite_build_seconds=162` in the old forced-reconfigure build path. A stripped
prepare-database plus PHPUnit-only comparison removed build, fetch, and
Composer phases and showed parity on host-`/tmp` storage: main `4760d512`
reported PHPUnit `00:24.500` and `just_phpunit_seconds=37`, while ownerless
head `287b6be4` reported PHPUnit `00:24.289` and
`just_phpunit_seconds=38`.

The same audit found no ordinary mysqli ownerless-mode leak:
`php_mysqli_mylite.c` still opens WordPress connections with
`MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE`, and direct/prepared ordinary SQL
continues to bypass ownerless statement tokens, statement gates, page-version
refresh, and transaction/read-view pin handling when `db->ownerless_rw_open` is
false. Ordinary non-memory opens still initialize fixed concurrency metadata,
shared memory, process-slot bookkeeping, page-log/checkpoint anchors, and the
core system-table serialization lock, so open/close-specific performance
regressions should be profiled in `start_runtime()`, the
`prepare_concurrency_*()` helpers, `map_concurrency_shared_memory_for_runtime()`,
`allocate_concurrency_process_slot()`, `ensure_core_system_tables()`, and the
matching close-side cleanup. The current focused WordPress `Tests_DB` runtime
does not show an ordinary per-statement regression versus main.

A 2026-06-06 audit after ownerless view crash-recovery slices through
`89d584e1` repeated the pinned `Tests_DB` comparison on the same machine and
host-`/tmp` database storage. Ownerless head `89d584e1` reported PHPUnit
`00:22.984`, `wordpress_phpunit_shell_real_seconds=38.493`, and
`wordpress_phpunit_seconds=39`; main `4760d512` reported PHPUnit `00:23.989`
and `wordpress_phpunit_seconds=39`. The branch had no post-parity changes in
the PHP adapter, WordPress harness, CI workflow, or `database.cc` ordinary SQL
hot path; the current evidence continues to point at setup/full-suite variance,
not an ordinary WordPress mysqli runtime regression.

A 2026-06-06 follow-up at current head `b38502b1` rechecked both CI and the
focused ordinary mysqli path after the native-hook shutdown reset. The latest
green full-suite WordPress CI job reported PHPUnit `18:05.762`,
`wordpress_phpunit_shell_real_seconds=1089.913`,
`wordpress_phpunit_shell_user_seconds=556.083`,
`wordpress_phpunit_shell_sys_seconds=393.555`, and
`wordpress_phpunit_seconds=1089`, while the previous slow ownerless samples at
`2120ccd7` and `8a35123e` reported about `1753s`/`1739s` with roughly `1005s`
of system CPU inside PHPUnit. A same-machine pinned `Tests_DB` comparison then
reported ownerless `b38502b1` at PHPUnit `00:22.464`,
`wordpress_phpunit_shell_real_seconds=34.004`, and
`wordpress_phpunit_seconds=34`, versus main `4760d512` at PHPUnit `00:22.455`
and `wordpress_phpunit_seconds=34` from a detached `/tmp` worktree. Current
evidence therefore shows parity with main for ordinary WordPress database work;
the visible CI variation is the full-suite runner band, cold setup/build work,
and transient embedded failures outside the WordPress job, not a remaining
ordinary SQL hot-path leak.

A later 2026-06-06 CI comparison after more ownerless slices reached the same
conclusion. Main `4760d512` run `26337270671` reported
`mylite_build_seconds=411`, PHPUnit `28:21.227`,
`wordpress_phpunit_seconds=1706`, and `wordpress_total_seconds=2155`. The
ownerless branch `cd09a810` run `27055919789` reported
`mylite_build_seconds=408`, PHPUnit `28:09.751`,
`wordpress_phpunit_shell_real_seconds=1694.760`,
`wordpress_phpunit_shell_user_seconds=602.585`,
`wordpress_phpunit_shell_sys_seconds=970.288`,
`wordpress_phpunit_seconds=1695`, and `wordpress_total_seconds=2142`. That
puts the branch slightly ahead of the comparable main CI sample; the older main
run `26334982122` at `777a2259` completed PHPUnit in `17:31.277`, so the
largest visible swing is the GitHub runner/full-suite band rather than an
ownerless branch regression.

A 2026-06-06 audit at ownerless head `c8614604` rechecked the latest
post-parity MariaDB edits. Since `b38502b1`, the ordinary WordPress-relevant
runtime delta was limited to stored-routine execution guards and ownerless
InnoDB hook fault/refresh fixes; the PHP adapter, WordPress harness, CI
workflow, and `database.cc` ordinary SQL path did not change. A first local
comparison showed ownerless `Tests_DB` at PHPUnit `00:21.205` with
`wordpress_phpunit_shell_real_seconds=40.701` and
`wordpress_phpunit_seconds=40`, while main `4760d512` from a detached `/tmp`
worktree reported PHPUnit `00:21.254` and `wordpress_phpunit_seconds=34`. The
extra wrapper time came from comparing a branch WordPress/PHPUnit tree on the
workspace filesystem with a main tree under `/tmp`, not from PHPUnit's own
database test timer. Repeating ownerless from a detached `/tmp` worktree
matched the main storage placement: cold ownerless reported PHPUnit
`00:22.085`, `wordpress_phpunit_shell_real_seconds=33.743`, and
`wordpress_phpunit_seconds=33`, while a warm ownerless rerun reported
`mariadb_embedded_configure=skipped`, `mylite_build_seconds=4`, PHPUnit
`00:23.164`, `wordpress_phpunit_shell_real_seconds=34.988`,
`wordpress_phpunit_shell_user_seconds=18.110`,
`wordpress_phpunit_shell_sys_seconds=16.075`, and
`wordpress_phpunit_seconds=35`. Full-suite ownerless CI stayed green in both
the fast and slow runner bands. Run `27059012651` at `25697f61` reported
`mylite_build_seconds=360`, PHPUnit `17:47.337`,
`wordpress_phpunit_shell_real_seconds=1071.532`,
`wordpress_phpunit_shell_user_seconds=552.882`,
`wordpress_phpunit_shell_sys_seconds=395.692`, `wordpress_phpunit_seconds=1072`,
and `wordpress_total_seconds=1469`; current-head run `27059744570` at
`c8614604` reported `mylite_build_seconds=377`, PHPUnit `29:09.152`,
`wordpress_phpunit_shell_real_seconds=1754.090`,
`wordpress_phpunit_shell_user_seconds=629.441`,
`wordpress_phpunit_shell_sys_seconds=1002.392`, `wordpress_phpunit_seconds=1754`,
and `wordpress_total_seconds=2179`. Current evidence keeps the ordinary
WordPress mysqli path at main parity and attributes the remaining apparent
slowness to storage placement, cold build/setup phases, and the known
full-suite CI runner band.

A follow-up 2026-06-06 audit at current ownerless head `09b95cc5` checked the
post-sequence-guard branch state. The only WordPress-adjacent code change since
the prior parity point was the `item_func.cc` sequence-expression guard, which
does not run on the ordinary WordPress database path. A detached `/tmp`
ownerless worktree reported `Tests_DB` PHPUnit `00:23.402` on a cold run and
`00:26.387` on the immediate warm run, with
`wordpress_phpunit_seconds=36` and `41` respectively. A same-machine detached
main worktree at `4760d512` reported PHPUnit `00:31.934` and
`wordpress_phpunit_seconds=61` on the older forced-reconfigure harness path.
The latest completed full WordPress CI sample at `27fd2113` reported PHPUnit
`27:48.011` and `wordpress_phpunit_seconds=1673`, still inside the documented
main full-suite runner band. The current branch therefore remains close to
trunk for ordinary WordPress database runtime; the remaining slow CI samples
continue to track runner/system-call variance and setup cost, not an ownerless
hot-path regression.

A later 2026-06-06 audit at ownerless head `49b5c0aa` checked the branch after
the FK row-step crash slice and ownerless SQL shard split. Running from the
active workspace with the database on host `/tmp` reported PHPUnit `00:24.517`,
`wordpress_phpunit_shell_real_seconds=56.235`, and
`wordpress_phpunit_seconds=56`; repeating from a detached `/tmp` ownerless
worktree removed the known workspace-source-tree storage confounder. The cold
detached run reported `mylite_build_seconds=433`, PHPUnit `00:24.377`,
`wordpress_phpunit_shell_real_seconds=37.522`, and
`wordpress_phpunit_seconds=38`; the immediate warm rerun skipped MariaDB
configure, reported `mylite_build_seconds=5`, PHPUnit `00:22.996`,
`wordpress_phpunit_shell_real_seconds=35.696`,
`wordpress_phpunit_shell_user_seconds=18.291`,
`wordpress_phpunit_shell_sys_seconds=16.284`, and
`wordpress_phpunit_seconds=36`. A same-machine detached main worktree at
`4760d512` using the older forced-reconfigure harness reported
`mylite_build_seconds=157`, PHPUnit `00:21.515`,
`wordpress_phpunit_seconds=37`, and `wordpress_total_seconds=241`. The current
focused evidence therefore keeps ownerless close to trunk for ordinary
WordPress database runtime; the visible wrapper differences continue to come
from workspace placement, build/setup behavior, host load, and the full-suite
runner band rather than an ordinary ownerless mysqli hot-path regression.

After the DDL seed-suite, external MariaDB replay, and multi-drop replay slices
through `aa414d91`, the same pinned WordPress `Tests_DB` filter was rechecked
before resuming broader ownerless work. The active workspace run again showed
the known source-tree placement artifact: PHPUnit itself reported `00:23.118`,
but `wordpress_phpunit_shell_real_seconds=47.944` and
`wordpress_phpunit_seconds=48` with `/work` on the workspace filesystem and
the database parent on tmpfs. A detached `/tmp` ownerless worktree at
`aa414d91` removed that placement difference and reported cold-build PHPUnit
`00:22.219`, `wordpress_phpunit_shell_real_seconds=35.500`, and
`wordpress_phpunit_seconds=35`. A same-machine detached main worktree at
`4760d512` reported PHPUnit `00:22.186` and `wordpress_phpunit_seconds=35`.
That keeps the ordinary WordPress mysqli database path at trunk parity after
the latest ownerless replay slices; slow-looking samples still need to be
read as setup, source/storage placement, or full-suite runner-band effects
unless PHPUnit's own timer moves with them.

After the native snapshot-boundary and transaction snapshot-retry slices
through `926c8952`, the WordPress-relevant source delta since `aa414d91` was
limited to ownerless InnoDB transaction snapshot hooks. The PHP adapter,
WordPress harness, CI workflow, and ordinary `database.cc` SQL path were
unchanged, and WordPress still opens `MYLITE_OPEN_READWRITE |
MYLITE_OPEN_CREATE` rather than ownerless mode. A current-head cold run rebuilt
the changed InnoDB archive and reported `mylite_build_seconds=78`, PHPUnit
`00:21.959`, `wordpress_phpunit_shell_real_seconds=42.375`, and
`wordpress_phpunit_seconds=43`; the immediate warm rerun skipped MariaDB
configure, did no native rebuild work, and reported `mylite_build_seconds=5`,
PHPUnit `00:22.336`, `wordpress_phpunit_shell_real_seconds=35.398`, and
`wordpress_phpunit_seconds=36`. The same pinned WordPress `Tests_DB` filter on
a detached `/tmp` main worktree at `4760d512` reported
`mylite_build_seconds=134`, PHPUnit `00:23.195`,
`wordpress_phpunit_seconds=37`, and `wordpress_total_seconds=205` on the older
forced-reconfigure harness path. Current focused evidence therefore keeps the
ordinary WordPress mysqli path at trunk parity after the latest ownerless
native hook changes; the visible branch slowdowns remain rebuild/setup,
source/storage placement, or full-suite CI runner-band effects unless
PHPUnit's own timer regresses.

After later ownerless DDL/crash evidence slices through `7310160e`, a detached
`/tmp` ownerless worktree was compared with a detached `/tmp` main worktree
using the same pinned WordPress ref and host-`/tmp` database placement. The
ownerless cold detached run had to build a new embedded archive and reported
`mylite_build_seconds=350`, but the focused PHPUnit body remained at
`00:21.718` with `wordpress_phpunit_shell_real_seconds=34.224`,
`wordpress_phpunit_shell_user_seconds=18.180`,
`wordpress_phpunit_shell_sys_seconds=15.107`,
`wordpress_phpunit_seconds=35`, and `wordpress_total_seconds=410`. The pinned
main worktree at `4760d512` reported `mylite_build_seconds=104` on the older
forced-reconfigure/no-Ninja-work harness path, PHPUnit `00:22.386`,
`wordpress_phpunit_seconds=34`, and `wordpress_total_seconds=157`. The current
head therefore remains close to trunk for ordinary WordPress database runtime;
the branch's much larger cold wrapper time in this probe was build cache state,
not PHPUnit execution.

After the column IF EXISTS crash-recovery slices through `f8c0ede7`, the same
pinned WordPress `Tests_DB` comparison was repeated from detached `/tmp`
worktrees with both databases on host `/tmp`. The cold ownerless run rebuilt
the detached embedded archive and reported `mariadb_embedded_configure=required`,
`mylite_build_seconds=366`, PHPUnit `00:23.061`,
`wordpress_phpunit_shell_real_seconds=34.355`, and
`wordpress_phpunit_seconds=34`. The immediate warm ownerless rerun skipped
MariaDB configure, reported `mylite_build_seconds=5`, PHPUnit `00:21.828`,
`wordpress_phpunit_shell_real_seconds=34.840`, and
`wordpress_phpunit_seconds=35`. The pinned main worktree at `4760d512` reported
`mylite_build_seconds=122`, PHPUnit `00:21.617`,
`wordpress_phpunit_seconds=33`, and `wordpress_total_seconds=179` on the older
forced-reconfigure harness path. The focused WordPress database runtime is
still close to trunk; the only large branch wrapper number in this probe was
the cold embedded rebuild in the detached ownerless worktree.

After the ownerless CTest runner-diagnostics slice at `b9947b0a`, the pinned
WordPress `Tests_DB` comparison was refreshed on the active ownerless
workspace with the database on host `/tmp` storage. Current ownerless head
rebuilt the touched MyLite library and reported `mylite_build_seconds=16`,
PHPUnit `00:21.540`, `wordpress_phpunit_shell_real_seconds=36.842`,
`wordpress_phpunit_shell_user_seconds=17.614`,
`wordpress_phpunit_shell_sys_seconds=15.890`, `wordpress_phpunit_seconds=37`,
and `wordpress_total_seconds=67`. A same-machine detached main worktree at
`4760d512` reported `mylite_build_seconds=100` on the older forced-configure
harness path, PHPUnit `00:22.394`, `wordpress_phpunit_seconds=36`, and
`wordpress_total_seconds=155`. The current ownerless branch remains at trunk
parity for focused ordinary WordPress database runtime; the main wrapper still
spends substantial time in repeated MariaDB configure, while the branch's
PHPUnit body is not slower than main.

After the ownerless SQL shard-balancing and embedded CTest scheduling slices
through `263eca20`, a detached `/tmp` ownerless worktree was compared with the
same detached `/tmp` main worktree at `4760d512` using the pinned CI WordPress
ref and host `/tmp` database placement. The ownerless cold run rebuilt the
embedded archive and reported `mylite_build_seconds=418`, PHPUnit
`00:23.933`, `wordpress_phpunit_shell_real_seconds=35.484`, and
`wordpress_phpunit_seconds=35`; the immediate warm rerun skipped MariaDB
configure, reported `mylite_build_seconds=5`, PHPUnit `00:24.164`,
`wordpress_phpunit_shell_real_seconds=36.581`,
`wordpress_phpunit_shell_user_seconds=19.619`,
`wordpress_phpunit_shell_sys_seconds=16.155`, `wordpress_phpunit_seconds=37`,
and `wordpress_total_seconds=60`. The same-machine main worktree reported a
no-compile but forced-reconfigure run with `mylite_build_seconds=128`,
PHPUnit `00:23.448`, `wordpress_phpunit_seconds=37`, and
`wordpress_total_seconds=189`. Current evidence keeps the branch at trunk
parity for focused ordinary WordPress database runtime; the visible total-time
difference is still build/setup policy, not slower PHPUnit execution.

After the later pressure-policy and schema-drop replay slices through
`d7b6e00a`, the same pinned `Tests_DB` comparison was repeated from a detached
`/tmp` ownerless worktree and the detached `/tmp` main worktree at
`4760d512`. The host was heavily loaded, with 18 cores and a load average near
18, and both source/database paths were on host `/tmp` storage. The ownerless
cold detached run rebuilt the embedded archive and reported PHPUnit
`00:23.284`, `wordpress_phpunit_shell_real_seconds=35.002`,
`wordpress_phpunit_shell_user_seconds=18.202`,
`wordpress_phpunit_shell_sys_seconds=15.943`, `wordpress_phpunit_seconds=35`,
and `wordpress_total_seconds=515`; the immediate warm rerun skipped MariaDB
configure, reported `mylite_build_seconds=5`, PHPUnit `00:22.940`,
`wordpress_phpunit_shell_real_seconds=34.066`,
`wordpress_phpunit_shell_user_seconds=17.229`,
`wordpress_phpunit_shell_sys_seconds=15.858`, `wordpress_phpunit_seconds=34`,
and `wordpress_total_seconds=54`. The same-machine main worktree reported
`mylite_build_seconds=119`, PHPUnit `00:23.204`,
`wordpress_phpunit_seconds=35`, and `wordpress_total_seconds=180` on main's
older forced-reconfigure harness path. Current focused evidence still does not
show a branch PHPUnit runtime regression; cold wrapper differences remain build
cache/configure policy and setup state. The full WordPress CI job for
`d7b6e00a` then completed successfully in the documented slow runner band:
`mylite_build_seconds=379`, PHPUnit `28:49.075`,
`wordpress_phpunit_shell_real_seconds=1734.217`,
`wordpress_phpunit_shell_user_seconds=617.303`,
`wordpress_phpunit_shell_sys_seconds=1000.762`,
`wordpress_phpunit_seconds=1734`, and `wordpress_total_seconds=2148`. That is
close to the earlier pinned main slow sample at PHPUnit `28:21.227` and
`wordpress_phpunit_seconds=1706`, and to the earlier ownerless slow sample at
PHPUnit `29:09.152` and `wordpress_phpunit_seconds=1754`.

Before resuming CI-green work at ownerless head `b13f710c`, the performance
audit was refreshed again. The source delta from the latest parity record
`a8585f88` through `b13f710c` touched only ownerless docs, the ownerless
cross-process SQL test, and ownerless external seed-sweep tooling; it did not
change the PHP extensions, WordPress harness, CI workflow, ordinary
`database.cc` runtime, or MariaDB SQL/InnoDB files used by the WordPress mysqli
path. The current-head CI run `27090214928` completed green: embedded finished
in `9m47s`, and WordPress finished in `24m25s` with
`mylite_build_seconds=348`, `wordpress_dependency_seconds=8`, PHPUnit
`17:46.293`, `wordpress_phpunit_shell_real_seconds=1070.415`,
`wordpress_phpunit_shell_user_seconds=539.336`,
`wordpress_phpunit_shell_sys_seconds=392.165`,
`wordpress_phpunit_seconds=1070`, and `wordpress_total_seconds=1456`. That is
close to the pinned main fast-band `wordpress_phpunit_seconds=1055` sample and
well below the pinned main slow-band `wordpress_phpunit_seconds=1706` sample.
Current evidence therefore keeps focused `Tests_DB` and full-suite WordPress
PHPUnit close to the pinned main bands; future slowdown triage should require
PHPUnit's own timer or `wordpress_phpunit_seconds` to move before treating cold
setup, cancelled superseded pushes, storage placement, or unrelated CI job
failures as a branch runtime regression.

After the replacement-copy DDL pressure, table-wait negative-proof, active
reader pressure trace, and DDL lifecycle trace slices through `78df0ba7`, the
performance audit was refreshed before broader CI work resumed. The source
delta from `b13f710c` through `78df0ba7` did not touch the PHP extensions,
WordPress harness, CI workflow, ordinary `database.cc` runtime, or MariaDB
SQL/InnoDB files used by the WordPress mysqli path. A detached `/tmp`
ownerless worktree at `78df0ba7` using the pinned CI WordPress ref and host
`/tmp` database placement reported a cold run with
`mylite_build_seconds=436`, PHPUnit `00:25.521`,
`wordpress_phpunit_shell_real_seconds=39.268`,
`wordpress_phpunit_shell_user_seconds=20.035`,
`wordpress_phpunit_shell_sys_seconds=18.316`, and
`wordpress_phpunit_seconds=39`; the immediate warm rerun skipped MariaDB
configure, reported `mylite_build_seconds=5`, PHPUnit `00:22.700`,
`wordpress_phpunit_shell_real_seconds=34.831`,
`wordpress_phpunit_shell_user_seconds=18.089`,
`wordpress_phpunit_shell_sys_seconds=15.937`, and
`wordpress_phpunit_seconds=35`. Current-head focused WordPress database runtime
therefore remains close to the pinned main and ownerless parity band. The large
cold wrapper number was build cache state, while the warm PHPUnit body remains
the relevant regression signal for this branch.

After the stored-function trigger crash and storage-option DDL spelling slices
through `952f6083`, the performance audit was refreshed again. The source delta
from the latest parity record `fdc388b4` through `952f6083` touched ownerless
docs and `packages/libmylite/tests/ownerless_cross_process_sql_test.c`; it did
not change the PHP extensions, WordPress harness, CI workflow, ordinary
`database.cc` runtime, or MariaDB SQL/InnoDB files used by the WordPress mysqli
path. A current-head pinned `Tests_DB` run from the active ownerless workspace
with the database on host `/tmp` reported `mariadb_embedded_configure=skipped`,
`mylite_build_seconds=6`, `wordpress_dependency_seconds=4`,
`wordpress_prepare_db_seconds=1`, PHPUnit `00:23.808`,
`wordpress_phpunit_shell_real_seconds=47.209`,
`wordpress_phpunit_shell_user_seconds=21.201`,
`wordpress_phpunit_shell_sys_seconds=18.308`, `wordpress_phpunit_seconds=47`,
and `wordpress_total_seconds=65`. The same-machine main worktree at
`4760d512` using the same pinned WordPress ref and host `/tmp` database
placement reported `mylite_build_seconds=131`,
`wordpress_dependency_seconds=19`, PHPUnit `00:30.884`,
`wordpress_phpunit_seconds=58`, and `wordpress_total_seconds=222` on main's
older forced-configure harness path. Current evidence therefore does not
reproduce a branch PHPUnit runtime regression; the remaining slow-looking
numbers are still setup/build state, source placement, or full-suite runner
band effects unless PHPUnit's own timer moves outside the documented main band.

After the CI phase split and ownerless SQL shard slices through `d58e12b2`, the
performance audit was refreshed with a detached `/tmp` ownerless worktree and
the pinned CI WordPress ref. The split harness made the current phase costs
visible: cached Docker image build `4s`; cold setup `421s`, including
`mylite_mariadb_embedded_seconds=357`, `mylite_build_seconds=371`, and
`wordpress_dependency_seconds=33`; warm setup `30s`, with
`mariadb_embedded_configure=skipped`, `mylite_build_seconds=7`, and
`wordpress_dependency_seconds=3`; and database preparation
`wordpress_prepare_db_seconds=1` with `wordpress_total_seconds=4`. The isolated
branch PHPUnit phase for `--filter Tests_DB` completed 651 tests with PHPUnit
`00:21.784`, `wordpress_phpunit_shell_real_seconds=33.427`,
`wordpress_phpunit_shell_user_seconds=17.411`,
`wordpress_phpunit_shell_sys_seconds=14.994`, `wordpress_phpunit_seconds=34`,
and `wordpress_total_seconds=37`. A same-machine main `4760d512` run on the old
one-shot harness spent `mylite_build_seconds=132` and
`wordpress_dependency_seconds=12` before the test body, then reported PHPUnit
`00:26.911`, `wordpress_phpunit_seconds=44`, and `wordpress_total_seconds=209`.
The current focused WordPress database runtime is therefore still close to, and
in this sample faster than, the pinned main baseline; the large total-time
spread is setup/build visibility rather than a PHPUnit runtime cliff.

The same audit deepened the microbenchmarks. A patched branch `perf-probe`
using forwarded non-default iteration counts reported process startup
`94.331ms`, process plus MyLite connect/close `555.605ms`, `SELECT 1`
`196.21 ops/s`, transactional inserts `313.43 ops/s`, and primary-key point
selects `195.97 ops/s`. An inline main mysqli probe reported process startup
`162.715ms`, process plus connect/close `642.132ms`, `SELECT 1`
`244.61 ops/s`, transactional inserts `389.27 ops/s`, and point selects
`237.88 ops/s`. To separate mysqli wrapper/metadata overhead from the core
engine path, the audit also ran the lower-level `mylite` PHP extension:
ownerless reported query-per-call `SELECT 1` `254.98 ops/s` and prepared-reuse
`SELECT 1` `378.55 ops/s`, while main reported `250.91 ops/s` and
`387.91 ops/s`. The branch therefore does not show a per-process startup
regression or a core read-engine regression; if the simple mysqli microprobe is
optimized later, the likely target is mysqli result/field-metadata wrapping
rather than ownerless coordination leakage.

The `ordinary-ownerless-startup-performance-probe` slice turns that ad hoc
startup and engine-cost investigation into a first-party embedded probe.
`mylite_embedded_performance_probe` prints parseable ordinary and ownerless
open/close, direct `SELECT 1`, prepared `SELECT 1`, and transactional prepared
insert timings through the public C API. The embedded CI job runs it as a
separate visible step after the correctness tests, giving future performance
triage a compact per-process startup and core-engine signal outside the noisy
full WordPress PHPUnit runner band.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens ordinary
  WordPress mysqli connections with `MYLITE_OPEN_READWRITE |
  MYLITE_OPEN_CREATE`; it does not request `MYLITE_OPEN_OWNERLESS_RW`.
- `packages/libmylite/src/database.cc` dispatches direct SQL through
  `exec_impl()` and prepared SQL through `mylite_step()`.
- MariaDB/InnoDB hook sites in `mariadb/storage/innobase/mtr/mtr0mtr.cc`,
  `mariadb/storage/innobase/buf/buf0flu.cc`, and
  `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc` call
  the MyLite ownerless redo and page-version callbacks when installed.
- The branch had an ordinary-open hook leak: `start_runtime()` skipped the
  pre-startup InnoDB hook install for fresh ordinary opens, then later called
  `install_ownerless_runtime_hooks()` unconditionally after
  `mysql_server_init()`, reinstalling ownerless runtime, transaction,
  read-view, MDL, InnoDB lock, AUTO_INCREMENT, redo, and page-version hooks for
  WordPress-style ordinary opens.
- InnoDB mtr page-write paths are hot enough that even disabled ownerless
  helper calls matter, so hook-enabled predicates need to sit at call sites,
  not only inside the callback trampolines.
- A later `libmylite.embedded-same-process-concurrency` abort showed the same
  boundary also matters at close time: ownerless InnoDB hooks stayed installed
  through `mysql_thread_end()` and `mysql_server_end()`, so MariaDB shutdown
  mini-transactions could still enter ownerless page-write refresh callbacks
  after MyLite had already finished ownerless close-time work.

## Design

Keep ordinary exclusive `mylite_exec()` and `mylite_step()` on the native
MariaDB embedded path when `db->ownerless_rw_open` is false:

- continue running the existing unsupported-SQL policy before direct dispatch,
- execute direct SQL with `mysql_query()` and the existing result/diagnostic
  plumbing,
- execute prepared SQL with `mysql_stmt_execute()` and the existing
  bind/result/affected-row plumbing, and
- skip ownerless statement tokens, pressure checks, statement locks, page
  refresh, transaction/read-view page-version pinning, and post-statement
  ownerless checkpoint work. Ordinary direct SQL also avoids constructing the
  ownerless page-visibility cleanup scope.

Install the full InnoDB ownerless hook surface only for ownerless runtime opens
or for ordinary native exclusive reopen when retained page-version WAL payload
records exist. That keeps page-version reads available to the retained-WAL
recovery bridge while fresh ordinary application opens avoid ownerless InnoDB
lock, page-write, AUTO_INCREMENT, redo, page-visible, and page-version hook
dispatch. Enable redo/page-visible publication and page WAL appends only for
ownerless runtime opens: `MYLITE_OPEN_OWNERLESS_RW` or
`MYLITE_OPEN_SHARED_READONLY`.

Install ownerless runtime, transaction, read-view, and MDL hooks only for
ownerless runtime opens. Fresh ordinary exclusive opens own the process-wide
database lock, so they do not need ownerless shared-file deletion policy or
cross-process SQL registry hooks.

Keep ownerless native redo-recovery probes evidence-driven on the ordinary
startup path. A fresh ordinary WordPress child process should not read the
InnoDB redo prefix merely to discover there is no ownerless recovery bridge to
arm; native redo prefix capture is reserved for ownerless opens, retained page
WAL recovery, uncheckpointed native file-operation markers, or an existing
valid `mylite-redo-header.bin` backup.
Hook-only SQL coverage corrupts the saved redo-header backup boundaries and
proves malformed backups do not arm that ordinary-open recovery bridge.

When an ownerless writer starts `START TRANSACTION WITH CONSISTENT SNAPSHOT`
before any ownerless page-version payload exists, it may seed the durable
ownerless checkpoint and redo-visible baseline from the current native InnoDB
checkpoint LSN at snapshot-pin time. That gives active-reader boundary
synthesis a nonzero pin without reintroducing ordinary-open hook leakage or
running broad ownerless checkpoint seeding during MariaDB startup.

Ordinary exclusive opens still create and validate fixed directory-owned
coordination files. They do not append ownerless page-version payload records
for normal InnoDB writes, so the fixed `mylite-concurrency.wal` header remains
empty after ordinary DML.

On final runtime close, complete MyLite-owned ownerless cleanup before handing
control to MariaDB embedded shutdown: stop the checkpoint scheduler, run
close-time native checkpoint/page-log reclaim, and capture any no-live
redo-header repair evidence. Then reset the process-global ownerless
transaction, read-view, MDL, InnoDB lock, AUTO_INCREMENT, redo, and
page-version hooks before `mysql_thread_end()` and `mysql_server_end()` so
new MariaDB shutdown mini-transactions stay on the native path. Preserve the
native hook context structs until after `mysql_server_end()` because an InnoDB
mini-transaction may have cached the ownerless hook-enabled flag before the
reset; the mtr predicate rechecks the current process-global hook state before
entering ownerless callbacks, so stale cached mtrs finish on the native path
after reset. Keep the ownerless runtime shared-file deletion hook installed
until after `mysql_server_end()` so a closing live peer still honors the
process registry before deleting shutdown-time shared native files.
Ownerless page-write refresh is a no-return InnoDB mtr hook, so transient
redo/page-version probe failures are treated as no external page version being
available for that mtr rather than aborting native shutdown or same-process
embedded tests; ordinary durable replay/reopen paths remain responsible for
validating retained ownerless WAL state.

A 2026-06-07 profiler pass at ownerless head `dab0d01c` confirmed the CI
phase split already separates Docker image creation, setup/build, database
preparation, and the PHPUnit body. A same-machine pinned `Tests_DB` run showed
main `4760d512` at PHPUnit `00:21.743` with
`wordpress_phpunit_seconds=35`; the ownerless branch before this token fix
reported PHPUnit `00:23.229` with `wordpress_phpunit_seconds=39`. Manual
process probes showed ordinary PHP process startup and MyLite connect/close
were already effectively at main parity (`553.104ms` branch connect/close
versus `553.650ms` main), but steady SQL still carried avoidable branch-side
cost: `SELECT 1` `240.64 ops/s`, transactional inserts `327.70 ops/s`, and
point selects `220.35 ops/s`, versus main `262.60`, `380.87`, and
`236.36 ops/s`.

The remaining ordinary statement cost came from the enlarged
`SqlPolicyTokens` buffer used by ownerless policy analysis. Ownerless DDL needs
room for deeper token inspection, so the token array grew from the original
32 slots to 256 slots, but `collect_sql_policy_tokens()` value-initialized the
whole array for every ordinary direct statement, schema-tracking scan, and
ownerless policy scan. The collector now initializes only `count`; callers
already read only the first `count` populated `string_view` slots. After the
change, the same branch probe reported connect/close `540.899ms`,
`SELECT 1` `269.61 ops/s`, transactional inserts `368.15 ops/s`, and point
selects `249.22 ops/s`; the isolated pinned `Tests_DB` phase reported PHPUnit
`00:19.991`, `wordpress_phpunit_shell_real_seconds=31.760`, and
`wordpress_phpunit_seconds=32`.

## Scope And Non-Goals

In scope:

- Direct and prepared ordinary SQL hot-path gating.
- SQL policy token collection overhead on ordinary and ownerless statement
  scans.
- Runtime hook installation gating for fresh ordinary opens.
- Runtime hook gating for page-version/redo publication on ordinary native
  retained-WAL reopen.
- Regression coverage that ordinary InnoDB writes do not grow the ownerless
  page-version WAL payload.
- Regression coverage that an ownerless InnoDB close can be followed in the
  same process by an ordinary native reopen and write.
- WordPress `Tests_DB` performance comparison against the pinned main baseline.

Out of scope:

- Removing the fixed `concurrency/` metadata files from ordinary durable
  directories.
- Changing ownerless SQL semantics, pressure policy, shared read-only behavior,
  or the existing native exclusive retained-WAL read bridge.
- Further optimizing Docker image creation, Composer cache behavior, or
  WordPress dependency installation.
- Claiming full ownerless concurrency completion.

## Compatibility Impact

Ordinary exclusive embedded SQL behavior remains MariaDB-native. Ownerless
read/write and shared-readonly opens retain the coordination hooks needed for
their partial cross-process guarantees.

Application adapters that use ordinary read/write-create opens, including the
WordPress mysqli harness, should stay close to mainline embedded runtime
performance instead of paying ownerless coordination cost.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Ordinary durable opens still create
`concurrency/mylite-concurrency.meta`, `.lock`, `.shm`, `.wal`, and `.ckpt`
anchors as part of the current database directory lifecycle. The changed
runtime boundary is that ordinary InnoDB DML does not publish page-version WAL
records beyond the fixed `.wal` headers.

## Test Plan

- Add an embedded regression test that opens a normal exclusive database,
  creates an InnoDB table, performs DML, closes the handle, and asserts
  `concurrency/mylite-concurrency.wal` remains at the fixed empty header size.
- Run focused embedded direct/prepared/open-close coverage.
- Keep baseline `libmylite.embedded-open-close` timing separate from
  ownerless directory and product-hook SQL coverage with the
  `baseline`, `ownerless-directory`, and `ownerless-product-hooks` selectors.
- Run ownerless cross-process selectors that require page-version reads and
  writer coordination to prove ownerless mode still enables the hooks.
- Run the active-pin boundary selector plus instant-column variants to prove
  targeted native checkpoint seeding preserves snapshot boundaries without
  corrupting startup-time DDL metadata.
- Run the pinned WordPress `Tests_DB` harness and compare
  `wordpress_phpunit_seconds` plus PHPUnit's own elapsed time to main.
- Run process startup, process plus MyLite connect/close, steady `SELECT 1`,
  transactional insert, and point-select probes for branch and main.
- Run fair-path WordPress probes with the MyLite database on the same host
  storage for branch and main; otherwise DDL `fdatasync()` latency can dominate
  the comparison.
- Run format and diff whitespace checks.

## Acceptance Criteria

- Ordinary exclusive direct SQL and prepared SQL bypass ownerless statement
  machinery.
- Fresh ordinary exclusive opens do not install the ownerless InnoDB hook
  surface, runtime lifecycle hook, transaction hook, read-view hook, or MDL
  hook.
- Ordinary exclusive InnoDB writes do not append page-version WAL payload
  records.
- Ownerless/shared-readonly page-version selectors and native exclusive
  retained-WAL reopen coverage still pass.
- Ownerless final close clears process-global SQL/InnoDB hooks before MariaDB
  embedded shutdown begins, preserves native hook context until
  `mysql_server_end()` completes, and still preserves close-time page-log
  reclaim, redo-header repair work, and the shared-file deletion guard until
  shutdown is finished.
- Pinned WordPress `Tests_DB` runtime is close to the main baseline.
- SQL policy token collection does not zero-fill the full ownerless-sized token
  array before the populated token count is known.
- Full-suite WordPress CI timing remains in the same range as the pinned main
  baseline, and CTest reports baseline open/close separately from ownerless
  coverage expansion.

## Risks

- The hook predicates still exist in the InnoDB code path, but ordinary opens
  use inline enabled bits and guarded mtr call sites before shared-memory/WAL
  work and no longer append page-version payloads.
- The WordPress harness build phase is noisy and can still vary with local
  cache state or relink breadth. The runtime comparison must use
  `wordpress_phpunit_seconds` and PHPUnit's elapsed time, not only total wall
  time.
- WordPress `Tests_DB` is DDL-heavy. Comparing branches whose database
  directories are on different host filesystems can report a storage-latency
  regression instead of a code regression.

## Current Production Audit

A 2026-06-10 production audit at ownerless head `6693fadb` used guarded
production build directories, the CI-pinned WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56`, and host `/tmp` WordPress
database placement. The WordPress mysqli probe reported stock PHP startup
`55.655 ms`, MyLite-extension PHP startup `75.788 ms`, process plus MyLite
connect/close `603.801 ms`, in-process connect/close `393.118 ms`,
active-runtime reconnect `3.427 ms`, `SELECT 1` `380.95 ops/s`, point selects
`228.71 ops/s`, transactional inserts `385.62 ops/s`, prepared autocommit
inserts `315.41 ops/s`, and direct-string autocommit inserts `722.09 ops/s`.

The same prepared database and production PHP extension build ran the
test-only `^Tests_DB` shard with CI defaults for process-child profiling and
static `wpdb` scanning. PHPUnit reported `Time: 00:15.963` for `651` tests
with `3` skips; the harness reported shell real `27.582s` and
`wordpress_phpunit_seconds=27`. This keeps the ordinary WordPress database path
in the documented parity band. The remaining full-suite risk is repeated
process-isolated PHP/MariaDB embedded startup and shutdown, not ownerless hook
leakage into ordinary SQL execution.

A 2026-06-11 production CI run at ownerless head `d3b308eb` kept the split
timing signal visible after the BLOB pressure matrix slices. The WordPress
mysqli probe reported stock PHP startup `17.513 ms`, MyLite-extension PHP
startup `24.430 ms`, process plus MyLite connect/close `345.127 ms`,
in-process connect/close `313.520 ms`, active-runtime reconnect `2.035 ms`,
`SELECT 1` `1541.10 ops/s`, point selects `1394.55 ops/s`, transactional
inserts `1471.33 ops/s`, prepared autocommit inserts `1121.88 ops/s`, and
direct-string autocommit inserts `1180.44 ops/s`. The test-only shards reported
`^Tests_DB` shell real `9.468s`, deferred process-isolated shell real
`84.358s`, eager process-isolated shell real `60.579s`, and non-isolated
remaining shell real `691.743s`, for about `14.1` minutes across test-only
PHPUnit steps. The full WordPress CI job completed in `21m52s`; this remains
below the comparable main one-shot job that reported PHPUnit `28:21.227` and
`wordpress_phpunit_seconds=1706`.
