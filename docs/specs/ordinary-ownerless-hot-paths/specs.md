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

## Scope And Non-Goals

In scope:

- Direct and prepared ordinary SQL hot-path gating.
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
