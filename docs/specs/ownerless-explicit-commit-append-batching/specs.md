# Ownerless Explicit Commit Append Batching

## Problem Statement

Ownerless explicit transactions can prove eligible `INSERT ... VALUES` writes
through the later `COMMIT` statement and avoid the conservative history-page
flush fallback. The remaining COMMIT-time page-version publications include
transaction-deferred page images plus rollback-segment and undo history-proof
pages. Before this slice, the eligible COMMIT statement used the visible-fast
proof but did not enable page-log append batching for those COMMIT-time
publications, so the page log could take the append path once per published
record.

This slice makes eligible proof-backed COMMIT statements append-batch eligible
for the duration of the COMMIT statement only. It does not hold the page-log
append lock across user transaction work.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` implements
  `trx_t::write_serialisation_history()`. The MyLite ownerless path marks a
  transaction-local history proof before `mtr->commit()`, then accepts the fast
  path only when the rollback-segment and undo-header proof pages were
  published and no page-publication failure was observed.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes ownerless page versions
  through `mtr_t::ownerless_page_write_publish()` and the MyLite
  `mylite_ownerless_innodb_publish_page_version_with_flags()` callback.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  publishes transaction-deferred page images during
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()`, outside the
  normal mini-transaction page-publish loop.
- `packages/libmylite/src/database.cc` already maps pure `INSERT ... VALUES`
  statements to a visible-fast append-batch policy and releases any active
  batch from `OwnerlessStatementVisibleFastPathScope` before leaving the
  statement.
- `packages/libmylite/src/database.cc` already has
  `ownerless_transaction_commit_allows_visible_fast_path()` for proof-backed
  explicit COMMIT statements, but `ownerless_statement_fast_path_policy()`
  previously set only `visible_fast_path` for that branch.
- `packages/libmylite/src/database.cc` also routed external-snapshot-lineage
  page-log records through a direct append helper even when a page-publish
  batch was active.

## Design

- When `ownerless_transaction_commit_allows_visible_fast_path()` returns true,
  set both `visible_fast_path` and `append_batch_fast_path` on the COMMIT
  statement policy.
- Wrap `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` with the
  existing page-publish batch begin/end hooks so transaction-deferred page
  images can use the same page-log append session as the COMMIT history-proof
  mini-transaction.
- Add an external-snapshot-lineage session append variant that preserves the
  lineage record metadata flag while using the locked append-session writer.
- Keep the existing statement-scope release behavior. The batch begins lazily
  from the InnoDB page-publish batch hook on the first actual COMMIT-time page
  append. A non-deferred batch releases at the transaction-publish batch end;
  a proof-backed COMMIT batch stays open only until
  `OwnerlessStatementVisibleFastPathScope` leaves the COMMIT statement.
- Do not change the ownerless history proof, page-version record format, page
  checksum, checkpoint publication, page-index publication, or conservative
  fallback conditions.

## Compatibility And Storage Impact

SQL behavior and MariaDB transaction semantics are unchanged. The optimization
only groups page-log appends that were already part of the same proven COMMIT
statement. Durable record ordering and payload bytes remain unchanged, and the
page-log append session still writes payloads before record headers.

The page-log append lock is not held across user transaction statements. It is
held only while COMMIT-time ownerless page publications append records.

## Test Plan

- Extend the explicit ownerless transaction history/undo-elision coverage to
  enable page-log append perf counters around an eligible explicit transaction.
- Add production-probe-shaped small-row prepared explicit insert coverage so
  transaction-deferred page images are included, not only rollback-segment and
  undo history-proof pages.
- Add primitive page-log coverage for external-snapshot-lineage session
  appends preserving lineage metadata while counting as session appends.
- Assert that COMMIT-time explicit transaction page-log records are appended
  through append sessions, with no direct appends in the proven path.
- Preserve existing assertions for visible-fast COMMIT publication, history
  proof pages, zero history-page flush fallback, same-handle visibility,
  ownerless reopen, and forced `.shm` native reopen.
- Reduced production probe evidence after this slice:
  `mylite_perf_ownerless_insert_txn_page_log_append_calls=12`,
  `direct_append_calls=0`, `session_append_calls=12`,
  `session_begin_calls=1`, and `session_end_calls=1`, with COMMIT visibility
  still fast (`fast=1`, `flush=0`) and transaction page publication preserved
  (`transaction_image_published_per_transaction=3.000`,
  `transaction_buffer_published_per_transaction=7.000`).

## Acceptance Criteria

- Eligible explicit COMMIT statements use a page-log append session for their
  page-version publications.
- Savepoint-disqualified explicit transactions remain conservative.
- The optimization is statement-scoped and does not hold append locks across
  user transaction work.
- Existing ownerless explicit transaction recovery and reopen coverage still
  passes.
