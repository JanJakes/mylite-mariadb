# Ownerless Explicit Clean Page-Write Release

## Problem

`libmylite.ownerless-cross-process-sql.2` exposed a deterministic timeout in
`test_two_processes_update_different_innodb_tables`: one explicit ownerless
transaction updated `app.ownerless_a` and paused before commit, while a second
explicit transaction updated `app.ownerless_b`. The second process never
reached its ready pipe.

Live shared-memory inspection showed the second process waiting in the
page-write registry on the first process's dirty `(space=5,page=3)` page for
`ownerless_a`, even though its SQL target was `ownerless_b` (`space=6`). The
wait came from explicit-transaction clean preread/prepare paths and retained
clean transaction-deferred page-write records, not from a user-visible InnoDB
row/table conflict.

## Source Findings

- MariaDB/InnoDB handler statement end runs through
  `ha_innobase::external_lock(F_UNLCK)` in
  `mariadb/storage/innobase/handler/ha_innodb.cc`. MyLite already released
  transaction page-write gates there for explicit transactions after the last
  table left the statement.
- Ownerless page-write acquisition and deferred transaction ownership live in
  `mariadb/storage/innobase/mtr/mtr0mtr.cc`. `ownerless_page_write_enter()`
  records transaction-deferred pages in
  `trx_t::mylite_ownerless_modified_pages`; actual dirty pages are recorded
  separately in `trx_t::mylite_ownerless_dirty_pages` and page images are kept
  in `trx_t::mylite_ownerless_page_images`.
- Buffer preread page-write locking lives in
  `mariadb/storage/innobase/buf/buf0buf.cc`. For explicit SQL writers, this
  path can run before the page is known to be dirtied and can encounter clean
  pages from other open tablespaces.
- The shared page-write primitive in
  `packages/libmylite/src/ownerless_innodb_lock_registry.cc` models per-page
  X resources plus synthetic per-space and global gates. Dirty page ownership
  must stay transaction-scoped; clean preread/prepare ownership must not
  serialize independent explicit transactions.

## Design

- Keep dirty ownerless page-write locks transaction-scoped until commit or
  rollback.
- Keep autocommit DML/DDL on the existing conservative prepare/refresh path,
  because replacement DDL and tablespace replay depend on blocking refresh
  semantics.
- For explicit non-autocommit preread/prepare paths, avoid adding broad
  transaction gates once the statement already has a page-write gate.
- For explicit non-autocommit preread/prepare page locks after a gate already
  exists, use a zero-timeout untracked page-write attempt. A conflict means the
  clean preread refresh is skipped; a later real dirty path still acquires the
  tracked page-write lock and refreshes before marking the page dirty.
- Treat zero-timeout page-write attempts as true probes in the MyLite hook:
  they return on conflict without publishing a shared-registry waiting slot.
- At explicit statement end, release transaction-deferred page-write records
  that are neither dirty pages nor captured transaction page images. Retain
  gates until the existing gate cleanup and retain dirty/image-backed pages
  until transaction cleanup.
- Stop automatic escalation from a second per-space gate to the global gate.
  The global gate remains available for callers that explicitly request it,
  while ordinary explicit statements keep per-space gates and page-level dirty
  locks.

## Compatibility Impact

The slice preserves MariaDB/InnoDB row-lock semantics and narrows only MyLite's
ownerless physical page-write bridge. Independent explicit transactions that
update different InnoDB tables can both reach their post-update application
barriers before either commits. Conflicting updates to the same page or row
still use page-write and record/table lock waits, and autocommit DDL replacement
coverage remains conservative.

No public C API, wire protocol, durable file layout, or SQL syntax changes.

## Tests

- `different-tables` covers the original hang directly.
- `commit-race` covers concurrent explicit writers opening the same directory
  while peers hold independent dirty pages, and verifies non-SQL preread probes
  do not leave shared-registry waiters behind.
- `sql-weighted-shard 2 16` covers the direct regression plus nearby
  page-refresh, explicit transaction history-proof, live reclaim, compressed
  BLOB, replacement tablespace replay, generated-column policy, dictionary
  refresh, trigger DDL, text/blob prefix index DDL, and unsupported flush-lock
  policy cases.
- `create-or-replace-tablespace-replay` guards the autocommit DDL replacement
  path that failed when prepare-only nonblocking behavior was too broad.

## Acceptance Criteria

- The direct independent-table selector finishes without waiting for the first
  transaction to commit.
- The original CI shard finishes without timeout or replacement-table replay
  regression.
- Dirty page-write ownership and captured transaction page images remain held
  to transaction cleanup.
- Docs and compatibility notes state that clean explicit preread/prepare pages
  are not transaction-lifetime page-write locks.

## Risks

Broader multi-table explicit DML, DDL/file lifecycle recovery, native
redo/checkpoint reconciliation, and external MariaDB/RQG stress remain planned
ownerless concurrency gaps. This slice is scoped to false physical-page
serialization in explicit SQL writer preread/prepare and statement-end cleanup.
