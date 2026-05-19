# Active Append Write Buffer

## Problem

The current hot update path appends a replacement row page, a row-state page,
and replacement index-entry pages as one contiguous write per updated row.
That removed several page writes per update, but large explicit transactions
still pay one `pwrite()` per row update. Local storage-smoke profiling shows
the prepared primary-key update benchmark dominated by that append write.

MyLite needs a pager-like write path before it can approach SQLite-like row
write throughput. This slice adds a bounded active-checkpoint append buffer for
contiguous unpublished page runs without changing the durable file format.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc` documents that engines register in statement and
  normal transaction lists through `trans_register_ha()` and that
  `trans_commit_stmt()` / rollback hooks drive statement commit and rollback.
- `mariadb/sql/handler.cc:3193-3266` drives handlerton savepoint set, rollback,
  and release callbacks for registered engines.
- `mariadb/storage/mylite/ha_mylite.cc:2354-2377` begins the MyLite durable
  transaction checkpoint for active SQL transactions, begins a nested statement
  checkpoint for row DML, and registers the MyLite handlerton in the MariaDB
  statement and transaction lists.
- `mariadb/storage/mylite/ha_mylite.cc:706-768` finishes MyLite savepoint
  frames and transaction checkpoints through the same storage checkpoint API.
- `packages/mylite-storage/src/storage.c` keeps active statement headers in
  memory, publishes page `0` at checkpoint commit, and restores header/catalog
  pages plus truncates the primary file on rollback.
- `packages/mylite-storage/src/storage.c` writes the common inline update page
  run through `write_pages_at_raw()`, so buffering that function covers the
  measured update syscall hotspot while avoiding broad changes to catalog,
  BLOB/TEXT, and autoincrement writers.

## Design

Add a bounded transient append-page buffer to active storage checkpoints.

- Buffer only full-format contiguous append runs written through
  `write_pages_at_raw()`.
- Select the outermost active checkpoint for the primary file as the buffer
  owner. This lets a transaction batch successful nested statement commits.
- Require the run's first page id to equal the current active checkpoint page
  count. Non-append or sparse writes use the existing direct `pwrite()` path.
- Keep the buffer bounded and flush it as one larger contiguous write when it
  reaches the page limit or when the top-level checkpoint commits.
- Teach `read_page_at()` to serve pages still present in the active append
  buffer before falling back to the primary file. Flushed pages remain readable
  from the file through the existing path.
- On rollback, flush any retained buffered prefix that must survive the target
  page count, discard buffered pages beyond the restored checkpoint, and then
  let the existing header/catalog restore and file truncation hide or remove
  rolled-back appended pages.

The first buffer is intentionally simple and per-checkpoint. It is not a full
pager, does not add dirty-page eviction, and does not introduce a WAL file.

## Affected Subsystems

- MyLite storage checkpoint ownership and rollback.
- Active row-DML transaction and savepoint paths.
- Inline update append writes.
- Storage-smoke transaction tests and local performance baseline.

## Compatibility Impact

SQL behavior does not change. MariaDB continues to drive statement,
transaction, and savepoint boundaries through the existing MyLite handlerton
hooks. Reads in the owning checkpoint see their uncommitted appends; other
handles and processes keep using the transaction-start journal snapshot.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file plus the existing MyLite
journal companions. The append buffer is process memory only. Commit flushes
buffered pages before publishing the header page. Rollback discards unpublished
buffered pages and truncates the primary file to the restored checkpoint view.

## Public API And File-Format Impact

No public API or durable file-format changes.

## Storage-Engine Routing Impact

No routing policy changes. Tables routed from `InnoDB`, `MyISAM`, `Aria`, and
omitted/default engines continue to use MyLite storage.

## Binary-Size Impact

Small first-party C code only. No new dependency.

## Tests And Verification

- Add a transaction regression that performs enough indexed row updates inside
  one explicit transaction to cross the append-buffer flush threshold, rolls
  back to a savepoint, commits the retained prefix, and verifies state before
  and after reopen.
- Run the storage-smoke build targets.
- Run focused storage/embedded checkpoint tests and the full storage-smoke
  `ctest` suite.
- Run local performance baseline at small and large row counts.
- Run `git diff --check`.

## Acceptance Criteria

- Successful transaction update loops batch append writes without changing the
  page format.
- Rollback to savepoint preserves updates before the savepoint and hides updates
  after it, including when the buffer has already flushed once.
- Top-level commit flushes buffered pages before header publication.
- Top-level rollback discards uncommitted buffered pages.
- Existing transaction, recovery, storage, and embedded tests continue to pass.

## Risks And Open Questions

- This slice still writes append-only row and index history; it does not solve
  free-space reuse or navigable index maintenance.
- BLOB/TEXT update payloads keep the existing per-page writer until that path
  has its own batching design.
- Full SQLite-like write throughput still needs a real pager/WAL design and
  maintained navigable indexes.
