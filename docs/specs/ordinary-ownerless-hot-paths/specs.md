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
CMake targets.

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

## Scope And Non-Goals

In scope:

- Direct and prepared ordinary SQL hot-path gating.
- Runtime hook installation gating for fresh ordinary opens.
- Runtime hook gating for page-version/redo publication on ordinary native
  retained-WAL reopen.
- Regression coverage that ordinary InnoDB writes do not grow the ownerless
  page-version WAL payload.
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
- Pinned WordPress `Tests_DB` runtime is close to the main baseline.

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
