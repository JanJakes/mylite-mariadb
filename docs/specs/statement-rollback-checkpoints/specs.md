# Statement Rollback Checkpoints

## Problem

MyLite storage currently commits each append-only row, row-state, index,
autoincrement, and catalog mutation independently. That is crash-safe at the
individual storage API level, but it is not enough for MariaDB statement
semantics: a multi-row `INSERT`, `UPDATE`, CTAS, or DDL statement can write
some MyLite pages and then fail later in the same SQL statement. Those earlier
writes must not remain visible after the statement reports an error.

This slice adds a bounded checkpoint around file-backed MyLite SQL statements
executed through `libmylite`. It restores the committed header/catalog snapshot
when MariaDB reports statement failure, while continuing to reject explicit SQL
transaction control until full handlerton transaction hooks exist.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:trans_register_ha()` documents that storage engines
  must register with MariaDB's statement and transaction lists before
  `ha_commit_trans()` / `ha_rollback_trans()` can drive engine commit and
  rollback hooks.
- `mariadb/sql/handler.h` defines handlerton `commit(THD *, bool all)` and
  `rollback(THD *, bool all)` hooks. `all=false` is the statement boundary,
  including autocommit statements.
- `mariadb/sql/transaction.cc:trans_commit_stmt()` and
  `trans_rollback_stmt()` call the handler layer with `all=false` for
  statement completion.
- `mariadb/storage/mylite/ha_mylite.h` still advertises
  `HA_NO_TRANSACTIONS`, and `mariadb/storage/mylite/ha_mylite.cc` does not
  register `mylite_hton` in `external_lock()`. Full MariaDB transaction hook
  integration is therefore a later slice.
- `packages/mylite-storage/src/storage.c` stores committed visibility in the
  primary header page and catalog root page. Row, row-state, index, and
  autoincrement pages appended after an older header page count are not part of
  the logical database when that older header is restored.
- Existing storage publication already journals overwrites of the header and
  catalog root for crash recovery. That journal is per mutation, not a logical
  statement undo log.

## Design

Add a MyLite storage checkpoint API for one thread-local file-backed statement:

- begin: open the primary file for update, take an exclusive primary-file
  lock, recover any pending publication journal, and save the current header
  page plus catalog root page;
- commit: close the checkpoint handle and release the statement lock;
- rollback: restore the saved catalog root page, restore the saved header page,
  fsync the primary file, close the checkpoint handle, and release the lock.

While a checkpoint is active, storage APIs in the same thread borrow the
checkpoint file handle instead of opening and locking a second descriptor. This
keeps the exclusive file lock held across the MariaDB statement without making
the handler pass a new storage context through every existing call.

The initial implementation created checkpoints in `libmylite` for all
file-backed SQL statements that could mutate MyLite storage. The follow-up
transaction-handler hook slice moves row DML (`INSERT`, `UPDATE`, `DELETE`,
`REPLACE`, and `LOAD`) onto MariaDB's statement transaction hook path. DDL and
catalog paths that do not reliably enter `external_lock()` continue to use the
outer `libmylite` checkpoint for `CREATE`, `ALTER`, `DROP`, `RENAME`, and
`TRUNCATE`.

If MariaDB execution fails, MyLite restores the active checkpoint before
returning the SQL error. If execution succeeds, the checkpoint is committed
through MariaDB's statement commit hook for row DML or after any required
schema-catalog synchronization for DDL. If schema synchronization fails after a
successful schema DDL statement, the checkpoint is rolled back so the durable
MyLite file returns to its previous state.

## Supported Scope

- Failed direct SQL statement rollback for file-backed MyLite storage
  mutations executed through `mylite_exec()`.
- Failed prepared statement rollback for file-backed MyLite storage mutations
  executed through `mylite_step()`.
- MariaDB statement transaction hook ownership for covered row-DML checkpoint
  commit and rollback.
- Row, row-state, index-entry, autoincrement, and catalog visibility rollback
  by restoring the statement-start header/catalog snapshot.
- Cross-process protection while the statement is executing: the checkpoint
  holds the same primary-file exclusive lock used by storage writes.
- Continued explicit rejection of `BEGIN`, `COMMIT`, `ROLLBACK`,
  `SAVEPOINT`, `SET autocommit`, `SET TRANSACTION`, and `XA`.

## Non-Goals

- Full MariaDB handlerton `commit` / `rollback` / savepoint integration.
- Multi-statement transactions, nested transactions, savepoints, or XA.
- Crash recovery of a logical statement that has failed but is interrupted
  while the in-process rollback is restoring the saved pages.
- Lock manager redesign or concurrent writer scheduling beyond the existing
  coarse primary-file lock.
- Physical compaction of pages made unreachable by statement rollback.
- General undo logging for future B-tree or multi-page catalog structures.

## Compatibility Impact

Routed `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and default-MyLite
statements now get MariaDB-style failed-statement visibility for covered
single-statement DML and DDL paths. This is still not SQL transaction support:
explicit transaction-control statements remain rejected, and MyLite does not
claim `COMMIT`, `ROLLBACK`, or savepoint compatibility yet.

The first compatibility evidence targets partial writes that were observable
before this slice: multi-row inserts that fail after a prior row write, CHECK
constraint failures after an earlier row write, and unique-key update failures
after a prior replacement row/index publication.

## Single-File And Embedded-Lifecycle Impact

No new durable companion file is introduced. The checkpoint keeps the primary
`.mylite` file descriptor open for the duration of one `libmylite` statement
and uses the existing primary-file advisory lock. Clean statement commit or
rollback leaves only the primary file plus the already documented temporary
publication journal lifecycle.

The checkpoint state is process memory. If the process crashes during a
statement, existing per-mutation recovery keeps the primary file structurally
valid, but logical statement undo after that crash remains a later transaction
log/WAL slice.

## Public API And File-Format Impact

The public `libmylite` API does not change. The internal
`packages/mylite-storage` header gains an opaque checkpoint handle and
begin/commit/rollback functions for MariaDB-facing integration code.

The file format does not change. Rollback uses existing header and catalog root
page images.

## Storage-Engine Routing Impact

The checkpoint is above the storage-engine handler and below the public
`libmylite` SQL APIs. All supported requested engines that route to MyLite
share the same rollback behavior because they write the same primary file.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact should be limited to a small
storage checkpoint helper and a `libmylite` execution guard.

## Test And Verification Plan

- Add storage unit coverage for checkpoint commit and rollback over row,
  index, and catalog visibility.
- Add storage-engine smoke coverage for failed multi-row `INSERT`, failed
  CHECK-backed multi-row `INSERT`, and failed unique-key `UPDATE` on routed
  `ENGINE=InnoDB` tables.
- Verify close/reopen visibility after failed-statement rollback.
- Add a compatibility harness group for statement rollback.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests,
  the new harness group, and diff whitespace checks.

## Acceptance Criteria

- Covered failed SQL statements leave no visible rows, row-state changes,
  index entries, autoincrement state pages, or catalog records that were
  published after the statement checkpoint.
- Successful SQL statements keep their changes and release the statement lock.
- A second process cannot write the primary file while a statement checkpoint
  is active.
- Explicit transaction-control SQL remains rejected before MariaDB execution.
- Docs and compatibility tables describe statement rollback as partial and do
  not imply full transaction or savepoint support.

## Risks And Unresolved Questions

- The checkpoint is a bounded bridge, not the final transaction layer. Full
  MariaDB transaction semantics still need handlerton registration and logical
  undo/WAL design.
- Restoring header/catalog pages rolls back MyLite-visible autoincrement state.
  MariaDB/MySQL autoincrement gaps are engine-specific, so broader
  compatibility evidence is needed before claiming exact autoincrement rollback
  behavior.
- Crash during the rollback operation can still leave the file at the failed
  statement state or require existing page validation to catch corruption. A
  durable statement journal is required before claiming crash-safe logical
  statement undo.
