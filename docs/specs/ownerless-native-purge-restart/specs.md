# Ownerless Native Purge Restart

## Problem Statement

The ownerless foreign-key graph stress reducer exposed a stale no-live
ownerless reopen after worker processes committed and exited. A fresh process
opening the failed database directory read the correct native InnoDB state, but
the same parent process that had already opened and closed the embedded server
could read delete-marked foreign-key parent rows through the ownerless verifier.

The stale read was not durable file corruption. It was an embedded
same-process restart gap in MariaDB/InnoDB purge task state plus MyLite's
ownerless startup timing.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/srv/srv0srv.cc:1129` implements
  `purge_sys_t::wake_if_not_active()`. MyLite ownerless lock hooks deliberately
  suppress normal purge work and only clone the oldest view while the hooks are
  active. Ownerless read-view hooks must also defer physical purge, because the
  shared read-view registry can be the only proof that another process still
  needs InnoDB undo history.
- `mariadb/storage/innobase/srv/srv0srv.cc:1610` initializes purge tasks with
  `srv_init_purge_tasks()` and calls `purge_sys.coordinator_startup()`.
- `mariadb/storage/innobase/srv/srv0srv.cc:1616` disables the purge
  coordinator waitable task during shutdown through
  `srv_shutdown_purge_tasks()`.
- `mariadb/storage/innobase/trx/trx0purge.cc:133` creates `purge_sys`, while
  `trx0purge.cc:160` closes it. The object itself is static, so embedded
  same-process restart must reset transient members such as the purge queue,
  page map, and head/tail iterators.
- `mariadb/storage/innobase/trx/trx0purge.cc:968` through
  `trx0purge.cc:993` pops rollback segments from the purge queue and asserts
  that the popped segment still has a valid undo-log header page. A stale
  queue entry from an earlier embedded runtime can point at a reinitialized
  rollback segment whose `last_page_no` is `FIL_NULL`.
- `packages/libmylite/src/database.cc:start_runtime()` installs ownerless
  hooks before MariaDB startup for ordinary ownerless recovery, but can defer
  those InnoDB hooks when no live peers exist and no page-version WAL payloads
  remain. That no-live native path is the safe window to let MariaDB's native
  purge coordinator run before ownerless page/read hooks resume.
- A later FK graph reducer failure after this purge restart fix was a distinct
  ownerless MTR/page-write ownership issue, documented in
  `docs/specs/ownerless-mtr-modify-page-ownership/specs.md`; it must not be
  handled by broadening SQL duplicate-key retries or enabling live purge under
  ownerless hooks.

## Design

Reset MariaDB's process-local purge restart state on embedded startup:

- clear the purge queue, processed-page map, and head/tail iterators in
  `purge_sys_t::create()`;
- reset `purge_state.m_running` and re-enable the purge coordinator waitable
  task in `srv_init_purge_tasks()`.

Then, in MyLite ownerless no-live startup where current native redo is already
authoritative, defer InnoDB ownerless hooks until after MariaDB starts and run a
bounded native purge-settle pass before installing the hooks. The settle pass
does not require InnoDB history length to reach zero, because MariaDB can leave
non-purgeable history after a valid coordinator pass. It submits coordinator
passes until history is gone, stops decreasing, or the existing startup wait
timeout expires.

After native settle, MyLite refreshes the ownerless checkpoint/latest-visible
state from native InnoDB's current LSN and uses the existing post-start
ownerless hook installation path.

When ownerless read-view hooks are installed, MariaDB purge wake and coordinator
paths clone the oldest local plus directory-owned read view and return without
physical purge. This keeps long repeatable-read snapshots in one process from
losing undo history while independent writer processes repeatedly start,
commit, and close. The active-reader pressure regression coverage asserts both
the live process slot and the active read-view slot remain published throughout
the writer churn.

## Scope And Non-Goals

In scope:

- same-process embedded restart of InnoDB purge task state,
- no-live ownerless startup after worker commits and page-version WAL reclaim,
- live ownerless read-view retention while peer writers start and stop,
- FK graph stress verification across ownerless reopen, native reopen, and
  forced `.shm` rebuild.

Out of scope:

- claiming every native purge history shape is drained to zero,
- SQL-level table-lock fault injection,
- broader native redo/checkpoint reconciliation,
- broader DDL/file lifecycle recovery,
- external randomized MariaDB/RQG stress.

## Compatibility Impact

No SQL syntax or public C API changes. The observable compatibility change is
that same-process embedded reopen now behaves like a fresh process for the
covered no-live ownerless native recovery boundary, and active ownerless
repeatable-read snapshots do not lose required InnoDB history while peer
writers churn.

## Database Directory And Native Storage Impact

No directory layout changes. The fix keeps InnoDB native files authoritative
when no live peer and no retained page-version payloads remain, and avoids
installing MyLite ownerless page/read hooks until after the native purge
coordinator has had a chance to settle process-local recovered history. During
live ownerless operation, the directory-owned read-view registry is a native
purge boundary even when broader page/lock hooks are not the deciding signal.

## Test And Verification Plan

- Build the embedded MariaDB archive and focused ownerless SQL/primitives
  targets.
- Run `mylite_ownerless_cross_process_sql_test fk-graph-stress` with
  `MYLITE_OWNERLESS_FK_GRAPH_STRESS_ROUNDS=1` and `4`.
- Run `mylite_ownerless_cross_process_sql_test active-reader-pressure` with
  the default and an elevated `MYLITE_OWNERLESS_ACTIVE_READER_PRESSURE_ROUNDS`
  value.
- Run ownerless primitives and focused ownerless CTest/hook subsets.
- Run ownerless stress, format check, and `git diff --check`.

## Acceptance Criteria

- The FK graph reducer no longer reads deleted `kind=2` parent rows in the
  first same-process no-live ownerless verifier.
- A fresh ownerless opener, ordinary native opener, and forced `.shm` rebuild
  all observe the same FK graph totals.
- MariaDB purge coordinator startup does not assert on stale same-process
  purge queue entries.
- Active ownerless read-view pressure does not raise `DB_MISSING_HISTORY` while
  peer writers repeatedly update and close.

## Risks And Unresolved Questions

- Remaining nonzero InnoDB history after the settle pass is allowed when the
  coordinator no longer makes progress. The current slice proves the FK graph
  stale-read regression, not every possible purge-history shape.
- Broader native redo/checkpoint and DDL/file lifecycle recovery remain tracked
  in the ownerless cross-process concurrency spec.
