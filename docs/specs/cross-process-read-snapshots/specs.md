# Cross-Process Read Snapshots

## Problem

MyLite row-DML transactions currently hold an exclusive advisory lock on the
primary `.mylite` file for the full file-backed transaction. That is correct for
coarse writer safety, but it also makes a second process return busy for ordinary
reads while a cooperating writer has an active transaction.

The previous transaction-owner slice made same-process `libmylite` handles read
from the owning transaction's saved header and catalog snapshot. Cross-process
readers need the same bounded read-committed behavior without borrowing process
local checkpoint state.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::ha_external_lock()` enters the MyLite
  handler lock path for routed table statements.
- `mariadb/storage/mylite/ha_mylite.cc:external_lock()` starts MyLite statement
  or transaction checkpoints for row-DML work and keeps MariaDB transaction
  hooks registered until completion.
- `packages/mylite-storage/src/storage.c:mylite_storage_begin_transaction()`
  opens the primary file for update, takes an exclusive advisory lock, saves the
  transaction-start header and catalog root pages, and writes
  `<database>.mylite-transaction-journal`.
- `packages/mylite-storage/src/storage.c:open_existing_file()` performs pending
  journal recovery before taking a shared read lock. If an active writer owns the
  exclusive lock and the transaction journal exists, recovery returns busy and
  the read currently fails busy.
- `packages/mylite-storage/src/storage.c:begin_journal_at_path()` stores exactly
  the protected header page and, for transaction journals, the protected catalog
  root page. `validate_recovery_journal_pages()` verifies the journal pages and
  decodes the saved header.
- Current durable row, row-state, index-entry, autoincrement, and catalog
  publication is append-only relative to the saved transaction-start header. A
  reader using that saved header and catalog root page does not follow pages
  appended by the active writer.

## Design

When a read opens a file and pending journal recovery cannot acquire the
exclusive primary-file lock, MyLite will try a narrow transaction snapshot path:

- require a readable transaction journal;
- open the primary file read-only;
- verify that a nonblocking shared lock conflicts, which indicates that another
  process still owns an exclusive writer lock;
- read and validate the transaction journal;
- bind a thread-local read snapshot to the read-only primary file descriptor;
- serve reads of the header page and catalog root page from the journal snapshot;
- read all other pages directly from the primary file.

The path is intentionally read-only. Writes keep returning `MYLITE_STORAGE_BUSY`
while another process owns the transaction lock. If the shared-lock probe
succeeds, MyLite treats the journal as stale or needing recovery and does not use
it as a live snapshot.

## Affected Subsystems

- `packages/mylite-storage`: read open/recovery fallback, snapshot-bound page
  reads, storage tests.
- `packages/libmylite`: no public API change; routed SQL reads inherit the
  storage behavior.
- Architecture, compatibility, roadmap, and harness docs.

## Compatibility Impact

Cross-process readers of a cooperating MyLite writer can observe the
transaction-start committed state while the writer's transaction remains active.
This is a bounded read-committed-style guarantee for append-only durable row-DML
publication, not full InnoDB MVCC.

This does not add:

- concurrent writers;
- SQL lock semantics;
- isolation-level-specific read views;
- gap locks, deadlock detection, or lock wait diagnostics;
- WAL checkpointing or page-free-space reuse.

## DDL Metadata Routing Impact

No transactional DDL support is added. The snapshot includes the
transaction-start catalog root page so a cross-process reader does not discover
uncommitted catalog roots during an active row-DML transaction.

## Single-File And Embedded Lifecycle

No new durable companion is introduced. The slice reuses the existing transient
transaction journal and removes it through the existing commit, rollback, and
crash-recovery paths. Readers do not remove journals and do not perform recovery
unless they can acquire the existing exclusive recovery lock.

## Public API And File Format

The public `libmylite` API and primary `.mylite` file format do not change. The
transaction journal format is reused as-is.

## Storage-Engine Routing Impact

The behavior applies to durable MyLite-routed table reads, including requested
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted-engine tables that
resolve to MyLite, and explicit `ENGINE=MYLITE`. Runtime-volatile MEMORY/HEAP
rows remain process-local and are not covered by this cross-process path.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to storage read-open fallback
logic and one thread-local snapshot structure.

## Test And Verification Plan

- Add storage coverage proving that a child process can hold an active durable
  transaction with a transaction journal while the parent:
  - reads the transaction-start committed row and index snapshot;
  - cannot write while the child owns the writer lock;
  - sees the committed child row and index entry after the child commits.
- Preserve existing stale-journal recovery behavior: ordinary recovery journals
  and transaction journals without an active writer still require exclusive
  recovery rather than being mistaken for live snapshots.
- Run storage unit tests, storage-core compatibility harness, transaction
  compatibility harness, shell syntax checks, whitespace checks, and reject-file
  checks.

## Acceptance Criteria

- Cross-process reads no longer return busy solely because a cooperating MyLite
  writer has an active row-DML transaction journal and writer lock.
- Snapshot reads use the saved transaction-start header and catalog root page.
- Cross-process writes still return busy during the active writer transaction.
- Stale transaction journals are recovered by the existing exclusive recovery
  path and are not exposed as live snapshots.
- Documentation describes the bounded guarantee without claiming full MVCC.

## Risks And Unresolved Questions

- This relies on append-only publication for pages reachable from the saved
  header. Future free-space reuse requires read-view or WAL design before
  readers can safely ignore the writer lock.
- Advisory locks only protect cooperating processes. Non-MyLite writers can
  still corrupt the file, as before.
- The shared-lock probe distinguishes active exclusive writers from recoverable
  stale journals, but it does not identify the writer process as MyLite beyond
  the presence of a valid transaction journal.
