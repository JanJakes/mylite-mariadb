# Transaction Owner Isolation

## Problem

File-backed row-DML transactions keep their active storage checkpoint in
thread-local storage keyed by filename. That lets nested MyLite storage calls in
one statement or transaction borrow the locked file descriptor and see the
transaction's current uncommitted header page.

The filename-only lookup is too broad for `libmylite`: two database handles for
the same `.mylite` file can run on the same thread. A second handle must not
borrow the first handle's active transaction checkpoint and observe its
uncommitted inserts, updates, deletes, index entries, or autoincrement state.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::ha_external_lock()` calls storage-engine
  `external_lock()` when MariaDB takes table locks for a statement.
- `mariadb/sql/handler.cc:ha_commit_trans()` and `ha_rollback_trans()` call
  participating engine transaction hooks after engines register with
  `trans_register_ha()`.
- `mariadb/storage/mylite/ha_mylite.cc:external_lock()` starts MyLite statement
  checkpoints, starts an outer transaction checkpoint for explicit or
  autocommit-off transactions, and registers the MyLite handlerton for
  statement and transaction completion.
- `packages/mylite-storage/src/storage.c:active_statement_for()` currently
  matches only by filename. `open_existing_file()` and
  `open_existing_file_for_update()` then borrow the active checkpoint file for
  any caller on the same thread.
- `packages/mylite-storage/src/storage.c:mylite_storage_begin_transaction()`
  saves the transaction-start header and catalog root pages and holds the
  primary-file exclusive lock until commit or rollback.
- `packages/libmylite/src/database.cc` has one `mylite_db` object per public
  handle. Direct and prepared execution already route through handle-specific
  functions before calling MariaDB, which gives MyLite a natural owner token for
  storage checkpoint lookups.

## Design

Scope active storage checkpoints by caller owner as well as filename:

- add a thread-local storage context owner token;
- make `libmylite` set that token to the current `mylite_db` while it executes
  direct SQL, prepared SQL, savepoint controls, transaction checkpoint commit or
  rollback, and close-time transaction rollback;
- store the current owner token on each storage checkpoint frame;
- let same-owner operations keep borrowing the active checkpoint exactly as
  before, preserving nested statement and savepoint behavior;
- reject writes from a different owner while another owner has an active
  checkpoint for the same file;
- allow same-process, same-thread read operations from a different owner to use
  the active transaction's saved header and catalog root snapshot, so they see
  the transaction-start committed state rather than uncommitted pages.

The snapshot read path is deliberately narrow. It applies only to storage reads
inside the same process and thread while the owning checkpoint holds the file
lock. Cross-process readers still follow the existing advisory-lock behavior
and return busy while another process owns the exclusive writer lock.

## Affected Subsystems

- `packages/mylite-storage`: active checkpoint ownership, read-committed
  snapshot borrowing, and storage tests.
- `packages/libmylite`: handle-scoped storage context while executing SQL and
  finishing transactions/savepoints.
- `mariadb/storage/mylite`: no direct fork delta is required; raw embedded
  handler usage keeps the default null owner token.
- Architecture, compatibility, harness, and roadmap docs.

## Compatibility Impact

Two `libmylite` handles to the same file in one process no longer observe each
other's uncommitted durable row-DML transaction changes. A non-owning handle
sees the transaction-start committed snapshot until the owner commits or rolls
back.

This does not claim full InnoDB isolation:

- isolation-level assignments remain accepted compatibility setup SQL only;
- cross-process read progress during an active writer remains constrained by
  coarse file locks;
- concurrent writers still return busy;
- transactional DDL remains rejected;
- there is no MVCC, gap locking, deadlock detection, or WAL-based lock manager.

## DDL Metadata Routing Impact

No new transactional DDL is supported. Snapshot reads include the
transaction-start catalog root page so a non-owning handle does not see
accidental uncommitted catalog changes if a future path tries to write catalog
metadata inside a checkpoint.

## Single-File And Embedded Lifecycle

No new durable file or companion is introduced. The owner token is process-local
thread state, and the committed snapshot is the already-saved checkpoint header
and catalog root pages.

## Public API And File Format

The public `libmylite` API and primary `.mylite` file format do not change.
The internal storage API gains thread-local context-owner accessors.

## Storage-Engine Routing Impact

The behavior applies to durable MyLite-routed row storage, including
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and omitted-engine tables that
resolve to MyLite. Runtime-volatile MEMORY/HEAP rows already use separate
process-local snapshots and are not expanded by this slice.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future protocol wrapper should
inherit this behavior if each connection drives SQL through its own
`libmylite` handle or explicit storage context owner.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to owner-token checks and
snapshot read branching in first-party storage code.

## Test And Verification Plan

- Add storage tests proving:
  - same-owner reads see uncommitted transaction rows;
  - a different owner sees only the transaction-start committed snapshot;
  - a different owner cannot write while the first owner holds an active
    transaction checkpoint;
  - committed changes become visible to the second owner after commit.
- Add `libmylite` storage-smoke coverage with two simultaneous handles proving:
  - uncommitted inserts are hidden from the second handle;
  - uncommitted updates preserve the old value for the second handle;
  - uncommitted deletes preserve the old row for the second handle;
  - commit publishes changes and rollback hides them.
- Run storage unit tests, storage-smoke tests, transaction compatibility group,
  shell syntax checks, whitespace checks, and reject-file checks.

## Acceptance Criteria

- Active storage checkpoints are isolated by owner token.
- A non-owning same-thread handle never borrows another handle's uncommitted
  storage header.
- Same-process same-thread non-owning reads use the transaction-start committed
  snapshot.
- Non-owning writes fail busy while another owner owns the active checkpoint.
- Docs and compatibility tables describe the bounded guarantee without claiming
  full isolation semantics.

## Risks And Unresolved Questions

- The snapshot read path depends on append-only publication: restoring or using
  the checkpoint header hides pages appended after the transaction start. Future
  free-space reuse needs a richer read-view design.
- Same-process snapshot reads are not a substitute for cross-process MVCC.
  Cross-process readers still depend on the later lock-manager/WAL design.
- Raw embedded MariaDB handler usage outside `libmylite` keeps a null owner
  token. That matches current single-connection smoke coverage, but a future
  raw embedded multi-connection adapter should set an explicit owner token.
