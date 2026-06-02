# Ownerless Transient Page-Write Boundaries

## Problem

Ownerless cross-process InnoDB writers use directory-backed page-write locks to
serialize native page modifications across independent buffer pools. A stress
failure in the ownerless foreign-key graph test showed that a parent table could
end with different committed boundaries in its clustered and secondary indexes:
aggregate scans observed stale parent primary-key values while child rows were
current, and a diagnostic secondary-index lookup reported an InnoDB clustered
record mismatch.

The root cause has two parts. First, InnoDB can acquire a page-write lock before
`trx_t::id` is assigned, so MyLite gives that transaction a stable transient
page-write identity. The commit-boundary decision still required a nonzero
native `trx_t::id`, which let that first page write publish and release at
mini-transaction scope instead of remaining held and transaction-visible until
SQL commit. Second, after a transaction had already deferred one user page,
later X/SX-latched user pages in the same tablespace could skip ownerless
pre-write preparation while InnoDB was still navigating B-tree page-linked
state. That left the eventual modify path too late to refresh a stale clustered
or secondary-index page.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0trx.h` stores
  `trx_t::mylite_ownerless_page_write_trx_id` as the MyLite page-write
  transaction identifier retained until page-write locks are released even if
  the native `trx_t::id` changes later.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  assigns `mylite_ownerless_page_write_trx_id` in
  `page_write_transaction_id()` from the normal lock identity, creating a
  transient identity when `trx_t::id` is not available yet.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` uses
  `mtr_t::ownerless_page_write_uses_transaction_release()` to decide whether a
  persistent page write is held in `trx_t::mylite_ownerless_modified_pages` for
  transaction commit or published immediately at mini-transaction commit.
- `mtr_t::ownerless_page_writes_publish()` and the modified-page release path
  already add transaction-visible page writes to the transaction page list and
  defer lock release; the missing condition was recognizing the transient
  page-write transaction identity as sufficient ownership.
- `mtr_t::ownerless_page_write_should_prepare()` gates ownerless page-write
  acquisition before X/SX page-linked access. For persistent user pages, the
  previous policy skipped this prepare path after the transaction already had a
  modified page, relying on the later dirty-page path to acquire ownership.
  Cross-table pages still avoid unnecessary pre-locking so native row-lock
  deadlock detection can choose a victim for ordinary opposite-order row
  updates.
- `mariadb/storage/innobase/trx/trx0trx.cc` publishes transaction page versions,
  flushes dirty pages through the ownerless commit LSN, and releases shared
  page-write locks during transaction commit when ownerless page-write state is
  present.

## Scope And Non-Goals

In scope:

- Treat a nonzero `mylite_ownerless_page_write_trx_id` as transaction-scoped
  ownerless page-write ownership for explicit/non-autocommit transactions even
  when native `trx_t::id` is still zero.
- Prepare later persistent user pages in an already-modified tablespace before
  ownerless X/SX page-linked access in write-capable statements.
- Keep user data/index page versions invisible until SQL commit/rollback.
- Preserve existing lock-only, read-only, startup/recovery, and dictionary DDL
  exclusions.
- Use the deterministic ownerless foreign-key graph stress as the regression
  oracle because it mutates parent clustered pages, parent secondary indexes,
  and child referential-action pages in one transaction.

Out of scope:

- Replacing MariaDB/InnoDB native transaction ID assignment.
- Broad dirty-page publication for every DML commit.
- External MariaDB/RQG stress orchestration.
- New file-format or directory-layout behavior.

## Design

`mtr_t::ownerless_page_write_uses_transaction_release()` now returns true when
the current InnoDB transaction has either:

- a native nonzero `trx_t::id`, or
- a nonzero `trx_t::mylite_ownerless_page_write_trx_id` in an
  explicit/non-autocommit transaction.

The transient page-write identity is created by the ownerless page-write lock
acquisition path before a persistent page is modified. Once present, the first
dirty user data/index page follows the same transaction-visible path as later
pages: it is tracked in `mylite_ownerless_modified_pages`, held in the shared
page-write registry until commit/rollback cleanup, and published at the SQL
transaction visibility boundary instead of the mini-transaction boundary.
Autocommit statements keep the previous mini-transaction release path unless
InnoDB has assigned a native transaction ID, which avoids holding DDL/truncate
page-write locks while the peer waits at a test or application synchronization
point after the statement has completed.

`mtr_t::ownerless_page_write_should_prepare()` now prepares a persistent user
page when the transaction has no deferred user pages yet, when the statement is
autocommit, or when the transaction already has a real modified page in the same
tablespace. Startup/recovery, read-only transaction state, and plain SQL
`SELECT` still exclude the access. Cross-table pages after the first modified
tablespace keep the previous behavior so native record-lock waits, not
page-write waits, remain the deadlock authority for opposite-order row updates.
The page-write enter path still deduplicates pages already held by the current
mini-transaction or transaction, so repeated access to the same page does not
create duplicate registry entries.

This keeps cross-process readers and peer writers from observing or building on
a partial native page set from a transaction whose native ID had not yet been
assigned at the first page write, and it makes secondary-index navigation
within an already-modified tablespace refresh before using page-linked state
that may have changed in a peer process.

## Compatibility Impact

No SQL syntax, public C API, or native file-format compatibility changes. The
change strengthens ownerless write atomicity for native InnoDB tables by keeping
explicit-transaction first-page writes under the same commit-boundary rule as
later page writes while preserving autocommit DDL/DML release behavior.

## Directory And Lifecycle Impact

No directory layout changes. The existing directory-backed page-write lock
registry, page-version WAL, and checkpoint files are reused. Commit/rollback
cleanup remains responsible for releasing the transient page-write identity and
its held page-write locks.

## Native Storage Impact

Native InnoDB user data/index pages remain in MariaDB format. The slice changes
when MyLite publishes and releases ownerless visibility for the first dirty
persistent page of a transaction, preventing a peer process from using a
clustered/secondary index page boundary that belongs to only part of a SQL
transaction.

## Binary Size And Dependencies

No dependency changes. The production code change is a narrow conditional in
upstream-derived InnoDB mini-transaction code and does not add new linked
objects.

## Test Plan

- Rebuild the MariaDB embedded archive after the InnoDB source change.
- Build `mylite_ownerless_cross_process_sql_test` in the `ownerless-stress`
  preset.
- Run focused ownerless foreign-key graph stress.
- Run a bounded repeated FK graph stress loop.
- Run `child-failure-cleanup` to verify failing stress workers are reaped.
- Run the full `ownerless-stress` preset.
- Run focused embedded/hook ownerless selectors that cover page writes,
  transaction hooks, and live reclaim.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Concurrent FK graph workers no longer leave parent clustered and secondary
  index boundaries inconsistent.
- Final FK graph aggregate and referential oracles pass through ownerless and
  native exclusive reopen before and after forced `.shm` rebuild.
- First-page transaction writes and later same-tablespace X/SX-latched user
  pages still release on commit or rollback and do not broaden
  dictionary/startup/recovery behavior.
- The production fix is documented separately from the stress-harness cleanup
  that made failures fail fast.

## Risks And Follow-Up

- FK graph stress is deterministic but not a replacement for long-running
  external MariaDB/RQG stress.
- Broader DDL/file lifecycle recovery and external crash injection remain
  separate ownerless gaps.
