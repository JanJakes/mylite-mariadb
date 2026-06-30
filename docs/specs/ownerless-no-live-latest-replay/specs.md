# Ownerless No-Live Latest Replay

## Problem Statement

No-live ownerless recovery can be reached after a stale reader held an older
page-version boundary while later writers committed file-per-table DDL and DML.
The old replay path used the durable checkpoint-visible LSN as the target for
native tablespace replay. That is correct for live reader visibility, but it is
too old once the stale reader is gone and the runtime is rebuilding shared
ownerless state with no live processes left.

If native files are replayed only to that older visible boundary, a later
no-live checkpoint/reclaim can advance `.ckpt` to the latest committed LSN and
discard retained WAL while the moved native file still contains an older image.
The representative failure is a stale-reader `RENAME TABLE old TO moved`
followed by creating a different table at the original SQL name: the recreated
table can be current while the moved table remains at its pre-rename image.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `initialize_or_recover_concurrency_runtime()` detects dirty or stale
  directory-owned shared memory, distinguishes no-live stale-reader rebuilds,
  and either discards stale reader-only WAL with no user page records or calls
  `replay_concurrency_tablespaces()`.
- `packages/libmylite/src/database.cc`
  `replay_concurrency_tablespaces()` reads the durable checkpoint latest and
  visible LSNs, then applies page-log records to native InnoDB tablespaces
  through `mylite_ownerless_tablespace_replay_apply_with_flags()`.
- `packages/libmylite/src/database.cc`
  `release_runtime()` decides close-time ownerless shutdown mode and later
  native checkpoint/reclaim behavior based on retained page-log payload,
  native file-operation/DML markers, rollback-history obligations, and live
  peer state.
- The ownerless page-log replay primitive already selects latest visible
  records by commit LSN and preserves same-LSN native pages in product replay
  mode. The bug was the no-live target LSN, not page selection within that
  target.

## Design

Use the durable latest committed LSN, not the older visible LSN, when
`replay_concurrency_tablespaces()` runs during no-live shared-memory rebuild or
final ownerless close. With no live readers left, the old visible boundary no
longer protects any active snapshot. Native files must be brought forward to
the latest committed ownerless boundary before later `.ckpt` publication or WAL
reclaim can prove page-version records are redundant.

Retained WAL classification also stays conservative:

- retained payload WAL with any user data/index page records forces a real
  native checkpoint-oriented shutdown path even when an idle live peer remains;
- retained native-support-only rollback-history WAL can stay on the existing
  native-support retention path when exact native proof is not yet complete;
- same-name rename/create stale-reader replay accepts retained payload-bearing
  page-version WAL after final reopen as the safe fallback until broader
  native redo/checkpoint reconciliation proves truncation for that DDL class;
- the broader DDL tablespace replay family uses the same retained page-version
  fallback while still proving final table identity, rows, native files, and
  forced `.shm` rebuild visibility.

This slice does not claim that all retained DDL page-version WAL can now be
discarded. It prevents the unsafe opposite outcome: advancing durable recovery
metadata past a native file that has not been replayed to the same committed
boundary.

## Compatibility Impact

SQL results become more correct for already-supported ownerless DDL/DML
recovery shapes. Public C APIs, SQL syntax, wire protocol behavior, and durable
file formats do not change.

## Database Directory And Lifecycle Impact

No new files are introduced. Existing ownerless `.wal`, `.ckpt`, and `.shm`
files keep their formats. No-live stale-reader rebuilds may replay more
retained page-version records into native `.ibd` files before rebuilding
`.shm`, and final close may retain payload-bearing WAL longer when native proof
is incomplete.

## Native Storage Impact

Native InnoDB file-per-table pages are advanced to the latest committed
ownerless page-version boundary during no-live replay. Product replay still
keeps same-LSN native pages and ignores missing dropped tablespaces through the
existing replay flags.

## Build, Size, And Dependency Impact

No new dependency and no intentional production binary-size change.

## Test And Verification Plan

- Build the MariaDB embedded archive and `mylite_ownerless_cross_process_sql_test`
  with `php-embedded-prod`.
- Run the focused `rename-create-tablespace-replay` selector.
- Run the owning ownerless SQL shard and the full production ownerless SQL
  registered shard set serially.
- Run adjacent history-proof/native-support, CTAS post-create DML,
  active-reader pressure, killed active-reader pressure, and native file-op
  marker selectors.
- Run ownerless stress, production-build audit, format check, and
  `git diff --check`.

## Acceptance Criteria

- After a stale reader is killed, no-live ownerless recovery replays retained
  DDL file-per-table page versions to the latest committed boundary.
- Forced `.shm` rebuild and ordinary native reopen preserve the moved table's
  updated rows, secondary-index reads, and original `SPACE`.
- The recreated original-name table keeps its distinct `SPACE` and final rows.
- Payload-bearing user page-version WAL is either checkpointed or retained as
  page-version WAL; it is not silently treated as native-support-only evidence.

## Risks And Follow-Up

- This narrows the no-live stale-reader replay boundary; it does not complete
  all DDL/file-lifecycle recovery classes.
- Broader native redo/checkpoint reconciliation is still required to prove when
  payload-bearing DDL WAL can be truncated after final native replay.
- External MariaDB/RQG stress remains planned for randomized DDL/DML schedules.
