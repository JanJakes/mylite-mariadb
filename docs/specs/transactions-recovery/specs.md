# Transactions And Recovery

Status note: this slice introduced rollback-journal publication and recovery.
Later slices added statement rollback checkpoints, MariaDB statement hook
integration, bounded direct row-DML transactions, and supported direct session
autocommit mode. Savepoints, global or multi-assignment autocommit changes,
transaction modifiers, XA, transactional DDL, and fully transactional engine
flags remain outside the implemented scope.

## Problem

MyLite now publishes catalog, row, row-state, autoincrement, and index-entry
pages by appending new pages and overwriting the header, and some DDL paths also
overwrite the catalog root page. A crash between those writes can leave a torn
header or mismatched catalog generation. That is not compatible with the
single-file durability goal.

This slice adds the first bounded recovery guarantee: atomic publication of the
current append-only storage mutations through a MyLite-owned rollback journal.
It does not yet add SQL transaction registration, savepoints, multi-statement
rollback, or concurrent writer locking. Later transaction-control policy work
rejects those SQL surfaces explicitly until the handler hooks exist.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h` marks non-transactional engines with
  `HA_NO_TRANSACTIONS`. The current MyLite handler still advertises this flag,
  so SQL transaction and savepoint support must not be claimed in this slice.
- `mariadb/sql/handler.h` defines the transactional handlerton hooks through
  `commit(THD *, bool all)`, `rollback(THD *, bool all)`,
  `savepoint_set`, `savepoint_rollback`, and `savepoint_release`.
- `mariadb/sql/handler.cc:trans_register_ha()` documents the normal storage
  engine registration path. Engines commonly register from
  `handler::external_lock()`, and registration is idempotent.
- `mariadb/sql/handler.cc:ha_commit_trans()` and
  `ha_rollback_trans()` drive both statement and full transaction boundaries.
  That is the correct integration point for a later MyLite transaction layer.
- `mariadb/sql/transaction.cc` routes explicit `COMMIT`, `ROLLBACK`, and
  statement transaction cleanup to the handler commit/rollback layer.

## Design

Add a deterministic rollback journal companion named `<database>.mylite-journal`
for the current storage publication protocol. Before any mutating operation
overwrites committed primary-file pages, storage writes and fsyncs a journal
containing the original page images that may be overwritten:

- header page `0` for row, row-state, autoincrement, and index publications,
- header page `0` plus catalog root page `1` for catalog mutations.

The mutation then writes new append-only pages, overwrites the catalog page when
needed, overwrites the header, fsyncs the primary file, removes the journal, and
fsyncs the parent directory for journal create/remove durability. If a process
or OS crash leaves the journal behind, the next open restores the saved page
images, fsyncs the primary file, and removes the journal before validating the
header and catalog.

This gives an intentionally conservative first guarantee: after recovery, the
database returns to the previously committed state. Appended pages beyond the
restored header page count may remain physically present until free-space
reclamation exists, but they are not part of the committed logical database.

## Supported Scope

- Automatic rollback-journal recovery before any read or write open.
- Atomic publication of current append-only row, row-state, autoincrement, and
  index-entry writes.
- Atomic publication of current single-page catalog mutations.
- Corruption detection for malformed or checksum-invalid recovery journals.
- Deterministic cleanup after successful clean publication or recovery.

## Non-Goals

- SQL transaction support, `COMMIT`/`ROLLBACK` handlerton hooks, savepoints, or
  statement rollback.
- Locking for concurrent processes or multiple writers.
- WAL, checkpoints, group commit, or preserving partially completed commits
  after a crash. The first journal rolls back incomplete publication.
- Free-space reclamation for unreachable append-only pages.
- Multi-page catalog roots, B-tree pages, or page-level logical undo records.

## Compatibility Impact

Atomic commit and crash recovery move from planned to partial. At this slice's
point in history, rollback and savepoints remained planned because MariaDB's
SQL transaction hooks were not yet implemented and MyLite continued to
advertise `HA_NO_TRANSACTIONS`; public MyLite entry points rejected explicit
transaction control rather than claiming rollback behavior. Later slices added
statement rollback checkpoints and bounded direct row-DML transaction
checkpoints, but savepoints and fully transactional engine flags remain
planned.

`ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` requests that route to
MyLite get the same MyLite recovery behavior. They do not use durable
InnoDB/MyISAM/Aria recovery files.

## Single-File And File-Lifecycle Impact

The durable database state remains in the primary `.mylite` file. The
`<database>.mylite-journal` companion is MyLite-owned, deterministic, and
temporary. It is permitted only during an active mutating operation or after an
unclean interruption, and lifecycle tests must prove it is removed after clean
publication and after recovery.

## Test Plan

- Add storage tests for:
  - clean mutations removing the rollback journal,
  - recovery of a row/header publication journal,
  - recovery of a catalog/header publication journal,
  - rejection of corrupt journal headers.
- Extend lifecycle documentation to name the journal companion and its cleanup
  behavior.
- Keep embedded sidecar gates passing, proving clean operations do not leave
  forbidden durable engine files or stale MyLite journals.
- Run dev, embedded, storage-smoke, tidy, format, diff, and archive-size checks.

## Acceptance Criteria

- Every mutating storage API either publishes after fsyncing the primary file,
  removing the journal, and syncing the parent directory, or leaves a journal
  that restores the previous committed header/catalog state on the next open.
- Recovery runs before header validation so a torn primary header can be
  repaired from a valid journal.
- Corrupt journal files return `MYLITE_STORAGE_CORRUPT` and do not silently
  modify the primary file.
- Docs and compatibility tables describe the partial recovery guarantee and the
  remaining transaction/savepoint work.

## Risks

- Rolling back incomplete publication is simpler than preserving a completed
  commit, but it can discard a mutation that reached the primary file before
  journal removal. That is acceptable for this first bounded slice because the
  API only returns success after fsync and journal cleanup.
- Without file locks, two writers can still race. The next locking slice must
  prevent unsafe concurrent journal use before any cross-process write claims.
- Free-space reclamation remains necessary to reclaim append-only pages that
  become unreachable after rollback.
