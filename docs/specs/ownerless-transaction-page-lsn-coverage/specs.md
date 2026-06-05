# Ownerless Transaction Page LSN Coverage

## Problem Statement

Ownerless explicit transactions publish user data and index page versions at
SQL transaction commit/rollback so peer processes do not observe mixed
mini-transaction boundaries. The publication pass used the transaction commit
LSN as the maximum visible LSN and skipped any tracked page whose current
`FIL_PAGE_LSN` was greater than that value.

Foreign-key graph stress exposed a secondary-index drift shape after many
cross-process parent primary-key updates: the clustered parent row had advanced
to the next primary key, but the unique secondary index did not contain the
matching `(worker_id, kind, primary_key)` entry. InnoDB later reported that the
old secondary-index entry could not be found during a subsequent update. That
is a correctness failure, not a retryable duplicate-key or foreign-key error.

The same stress shape can also expose a silent current-read miss: after an
explicit transaction has local writes, ownerless statement refresh policy
stopped global refresh for non-`SELECT` statements. An `UPDATE ... WHERE id =
...` can therefore search a clean stale local B-tree page and affect zero rows
before the X/SX page-write prepare path has a chance to refresh the page. The
remaining statements in that SQL transaction can still update child tables,
leaving parent root rows behind child aggregate values.

## Source Findings

- `mariadb/storage/innobase/row/row0upd.cc`
  `row_upd_sec_index_entry()` delete-marks the old secondary-index entry and
  then calls `row_ins_sec_index_entry()` for the replacement entry. The failing
  diagnostic comes from the old-entry search path.
- `mariadb/storage/innobase/row/row0ins.cc`
  `row_ins_sec_index_entry_low()` uses independent mini-transactions for unique
  secondary duplicate scanning and insertion, so a single SQL transaction can
  have multiple page-modifying MTR LSNs before final commit.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::commit_log()` stamps dirty pages with each MTR commit LSN and
  records transaction-scoped ownerless pages in
  `trx_t::mylite_ownerless_modified_pages`.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::commit_persist()` derives the normal ownerless visibility LSN from
  the transaction commit MTR, then publishes tracked transaction pages, flushes
  dirty pages, and releases ownerless page-write locks.
- `mariadb/storage/innobase/trx/trx0roll.cc`
  full rollback publishes tracked transaction pages through the current redo
  LSN before releasing the transaction state.
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `buf_flush_publish_ownerless_page_to_lsn()` copied a tracked page but only
  published it when `page_lsn <= visible_lsn`.
- `packages/libmylite/src/database.cc`
  `refresh_ownerless_external_pages_before_statement()` performs ownerless
  statement-boundary refresh. Before this slice, `UPDATE`/`DELETE`/`INSERT` and
  locking reads did not request page-version reads, and an explicit transaction
  with local writes also disabled global refresh.

## Scope And Non-Goals

In scope:

- During transaction page publication, observe the current valid page LSN for
  every tracked real page.
- If any tracked page's valid `FIL_PAGE_LSN` is newer than the initial
  transaction visible LSN, use the highest observed tracked-page LSN as the
  ownerless visibility boundary for transaction page publication and flushing.
- Preserve existing transaction page-write tracking and same-tablespace
  page-write preparation policy.
- Refresh clean local pages for DML and locking-read current reads even inside
  explicit ownerless transactions after prior local writes.
- Keep duplicate-key, foreign-key, and other semantic SQL errors non-retryable
  in ownerless stress tests.

Out of scope:

- Broad dirty-page publication for ordinary DML commits.
- Cross-space pre-locking for all foreign-key action pages.
- Changing MariaDB/InnoDB secondary-index or foreign-key semantics.
- External MariaDB/RQG stress orchestration.

## Design

`buf_flush_publish_ownerless_page_to_lsn()` now returns the valid page LSN it
observed while copying a tracked page. It still publishes only when the page LSN
is at or below the supplied visibility LSN, and it still rejects mismatched page
ids or corrupt images before returning a nonzero value.

`mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` uses that observed
LSN to compute the maximum visible LSN proven by the transaction's own tracked
page set. The first pass preserves the previous fast path by publishing pages
that already fit under the initial visible LSN. When a tracked page proves a
newer LSN, the helper republishes the tracked pages under the widened boundary
and returns that boundary to the caller.

`trx_t::commit_persist()` and full rollback in `trx0roll.cc` then flush dirty
pages and publish the ownerless visible marker through the returned boundary.
The boundary is based on pages already held in the transaction page-write set,
so the slice does not widen page-lock acquisition or publish arbitrary buffer
pool pages for ordinary DML.

`database.cc` now treats DML and locking reads as ownerless current-read
statements for refresh purposes. They still do not enable repeatable-read
page-version snapshots like plain `SELECT`, but they do allow
`refresh_ownerless_external_pages_before_statement()` to evict or refresh clean
buffer-pool pages up to the ownerless visible LSN. Dirty local pages remain in
the buffer pool, preserving the current transaction's writes while making
subsequent `UPDATE ... WHERE ...` searches see peer-committed rows before they
can silently affect zero rows.

## Compatibility Impact

No SQL or public C API behavior changes. The slice preserves MariaDB-native
secondary-index, duplicate-key, current-read, and foreign-key behavior while
making ownerless cross-process page visibility match the transaction's complete
tracked page set and the current-read visibility expected by DML statements.

## Directory And Lifecycle Impact

No directory layout changes. The existing ownerless page-version WAL and
visible-LSN marker receive a visibility boundary that may be higher than the
initial transaction commit MTR LSN when tracked pages prove that boundary.

## Native Storage Impact

Native InnoDB page formats are unchanged. The slice only changes when MyLite
publishes already-tracked native page images for ownerless peer visibility.

## Binary Size And Dependencies

No new dependencies. The implementation changes existing ownerless publication
helpers and does not add public API surface.

## Test Plan

- Rebuild the embedded MariaDB archive and ownerless stress binary.
- Run `deadlock-rows` to guard row-lock deadlock handling.
- Run repeated 48-round `fk-graph-stress` loops to cover the secondary-index
  drift regression.
- Run the focused ownerless FK graph CTest selector.
- Run the full `ownerless-stress` preset.
- Run focused embedded ownerless SQL shards, hook ownerless checks, formatting,
  and diff whitespace checks before commit.

## Acceptance Criteria

- Repeated FK graph stress no longer reports unexpected duplicate-key,
  missing-secondary-entry, zero-row parent-update, or aggregate mismatch
  failures.
- Rollback and deadlock retry paths continue to publish and flush tracked
  transaction pages before releasing ownerless page-write locks.
- No new ownerless page-write pre-locking is introduced for cross-table pages.
- Compatibility docs continue to mark external MariaDB/RQG FK graph coverage as
  planned rather than completed.
