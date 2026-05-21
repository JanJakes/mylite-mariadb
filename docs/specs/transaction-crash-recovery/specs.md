# Transaction Crash Recovery

## Problem

Direct row-DML transactions now roll back correctly on explicit `ROLLBACK`,
close-time rollback, transaction restart, supported autocommit transitions, and
savepoint unwind. Those checkpoints are still process-local: if a process exits
after committing one or more statement checkpoints but before the outer direct
transaction commits, the primary file can retain uncommitted page-count
publication.

This slice adds a durable transaction recovery boundary for file-backed direct
row-DML transactions. It does not add WAL, isolation levels, concurrent
writers, transactional DDL, or handler-level transactional engine flags.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/transaction.cc:trans_commit()` and `trans_rollback()` drive
  whole-transaction handler boundaries through `ha_commit_trans()` and
  `ha_rollback_trans()`.
- `mariadb/sql/handler.cc:ha_commit_trans()` and `ha_rollback_trans()` clear
  participating engines only after the engine callbacks complete.
- `mariadb/storage/mylite/ha_mylite.cc` currently maps handler transaction
  boundaries onto MyLite storage checkpoints while the handler still advertises
  `HA_NO_TRANSACTIONS`.
- `packages/mylite-storage/src/storage.c:mylite_storage_begin_statement()`
  stores a checkpoint header/catalog snapshot in process memory and holds the
  primary-file lock until commit or rollback.
- `packages/mylite-storage/src/storage.c:begin_recovery_journal()` writes a
  short rollback journal to `<database>.mylite-journal` before mutating the
  header/catalog pages for a single storage publication, then
  `finish_recovery_journal()` removes it after flushing the primary file.
- `packages/mylite-storage/src/storage.c:recover_pending_journal_locked()`
  restores a pending single-publication journal before normal file use.

## Design

Add a storage-owned transaction journal separate from the existing
single-publication journal:

- `mylite_storage_begin_transaction()` opens the same storage checkpoint shape
  as `mylite_storage_begin_statement()`, but also writes a durable
  `<database>.mylite-transaction-journal` containing the transaction-start
  header page and catalog root page.
- Nested statement and savepoint checkpoints continue using in-memory
  `mylite_storage_begin_statement()` frames, so existing LIFO checkpoint rules
  stay unchanged.
- Handler transaction checkpoint starts also use the transaction journal entry
  point, while the handler still does not advertise full transactional engine
  flags.
- Commit flushes the primary file, removes the transaction journal, and fsyncs
  the parent directory. The journal removal is the durable commit point.
- Rollback restores the saved transaction-start pages, flushes the primary
  file, removes the transaction journal, and fsyncs the parent directory.
- Recovery first applies any pending single-publication journal, then applies a
  pending transaction journal. This ordering prevents a statement-start journal
  from resurrecting uncommitted transaction state after the transaction journal
  rolls back.

The transaction journal reuses the existing rollback-journal page format. The
filename and API entry point distinguish transaction recovery from
single-publication recovery without changing the primary `.mylite` file format.

## Affected Subsystems

- `packages/mylite-storage`: transaction journal path, recovery order, storage
  checkpoint API, storage tests.
- `mariadb/storage/mylite`: handler-started transaction checkpoints switch to
  transaction journals without changing advertised engine flags.
- `packages/libmylite`: direct transaction open path switches from statement
  checkpoint to transaction checkpoint for file-backed handles.
- Architecture, compatibility, API, harness, and roadmap docs.

## Compatibility Impact

An uncommitted file-backed direct row-DML transaction no longer becomes durable
after process exit. On the next open or storage read/write, MyLite restores the
transaction-start header/catalog state before exposing rows, indexes,
autoincrement state, row-state pages, or catalog records appended inside the
uncommitted transaction.

Compatibility remains partial:

- The recovery guarantee is for the current append-only checkpoint model.
- Transactional DDL remains rejected while a direct transaction is active.
- MEMORY/HEAP volatile rows and BLACKHOLE row-discard behavior do not expand
  the durable recovery claim.
- Handler-level transactional engine flags, isolation, WAL, and concurrent
  writers remain planned.

## DDL Metadata Routing Impact

No new DDL behavior is allowed. The transaction journal includes the catalog
root page so recovery remains safe if a future catalog mutation accidentally
enters the transaction path, but active-transaction DDL remains rejected in
`libmylite`.

## Single-File And Embedded Lifecycle

The transaction journal is a MyLite-owned transient recovery companion. It is
created only while a file-backed MyLite transaction checkpoint is active and is
removed on commit, rollback, close-time rollback, or recovery after an unclean
exit.

## Public API And File Format

The public `libmylite` C API and primary `.mylite` file format do not change.
The internal storage API gains a transaction checkpoint entry point.

## Storage-Engine Routing Impact

The guarantee applies to durable MyLite-routed row storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. Zero-file engines keep their
documented special behavior.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future protocol wrapper should
inherit this recovery behavior by using the public `libmylite` transaction
surface.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to transaction-journal path
handling and a small storage API branch.

## Test And Verification Plan

- Add storage tests proving:
  - transaction commit removes the transaction journal and keeps rows,
  - transaction rollback removes the journal and restores rows/indexes,
  - recovery removes a pending transaction journal and hides uncommitted rows,
  - recovery with both statement and transaction journals restores the
    transaction-start state.
- Add `libmylite` storage-smoke coverage proving a child process that exits
  with an active direct transaction does not make its row durable.
- Run dev, embedded, storage-smoke, transaction and crash-recovery harness
  groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- File-backed direct transactions create a durable transaction journal at
  transaction start.
- Commit and rollback remove the transaction journal only after making the
  final state durable.
- Pending transaction journals roll back uncommitted transaction changes on the
  next open/read/write.
- Existing statement journal recovery still works and is applied before
  transaction journal recovery when both exist.
- Docs and compatibility tables describe the recovery scope without claiming
  full transactional engine semantics.

## Risks And Unresolved Questions

- The transaction journal started by protecting only the header and catalog
  root pages. Current pager-backed dirty existing pages can extend it with
  transaction-start page preimages, but free-space reuse and broad multi-page
  mutations will require a richer transaction log.
- The commit point is transaction-journal removal, not a WAL frame. WAL remains
  the likely long-term path for concurrent writers and finer-grained recovery.
