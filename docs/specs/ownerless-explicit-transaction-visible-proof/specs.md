# Ownerless Explicit Transaction Visible Proof

## Problem Statement

Ownerless autocommit `INSERT ... VALUES` statements can publish committed page
versions without the conservative native dirty-page flush because MyLite marks
the statement as visible-fast while InnoDB commits the statement. Explicit SQL
transactions run their writes under one or more earlier statements and then
commit through a separate `COMMIT` statement. The previous undo-WAL elision
slice reduced pre-commit rollback-segment page-version WAL for that shape, but
the final `COMMIT` no longer had the statement-local visible-fast marker and
therefore recorded `flush_unproven_statement`.

This slice carries a conservative transaction-scoped proof from eligible
explicit-transaction writes to the later `COMMIT`, so a transaction made only of
already-proven ownerless `INSERT ... VALUES` writes can use the same
visible-fast commit publication gate as autocommit inserts.

## Source Findings

- Base: MariaDB 11.8.6 import `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/trx/trx0trx.cc` checks
  `ownerless_sql_command_allows_visible_fast_path(this)` while committing a
  transaction. The ownerless visible-fast path is accepted only when the commit
  has no recovery-LSN fallback, does not need the dirty-page bridge, has a
  page-write transaction id, has published deferred pages, has no page-publish
  failure, and has at least one published page.
- `packages/libmylite/src/database.cc` owns the SQL policy classifier and wraps
  MariaDB execution in `OwnerlessStatementVisibleFastPathScope`. That scope is
  statement-local, so an explicit transaction's `COMMIT` cannot inherit the
  earlier insert's marker without first-party transaction state.
- `packages/libmylite/src/database.cc` already tracks explicit transaction
  state through `ownerless_explicit_transaction_active`,
  `ownerless_transaction_has_local_write`, locking-read state, and
  `update_ownerless_transaction_state_after_successful_sql()`.
- `ownerless_insert_values_statement_allows_visible_fast_path()` accepts only
  `INSERT ... VALUES` statements with at least one row, and the higher-level
  visible-fast predicate also rejects foreign-key targets and dictionary states
  that require a conservative write.

## Design

Add first-party per-handle transaction proof fields:

- `ownerless_transaction_visible_fast_commit_candidate`: at least one
  successful local write in the current explicit transaction remained eligible
  for the existing visible-fast insert path.
- `ownerless_transaction_visible_fast_commit_disqualified`: a write or locking
  read in the current explicit transaction fell outside the bounded proof.

Before executing an explicit-transaction statement, update the proof:

- `INSERT ... VALUES` writes that pass the existing visible-fast predicate keep
  or establish the candidate when the transaction is not disqualified.
- Any other write disqualifies the transaction.
- Locking reads disqualify the transaction.
- `SAVEPOINT`, `RELEASE SAVEPOINT`, and `ROLLBACK TO SAVEPOINT` disqualify the
  transaction.
- Transaction-control statements do not themselves establish proof.

The existing statement-local visible-fast predicate becomes a two-part
predicate:

- ordinary visible-fast writes continue to use the existing insert/FK/dictionary
  checks;
- `COMMIT` is marked visible-fast only when the handle is still in an explicit
  transaction, the transaction has a local write, the candidate is true, the
  disqualified flag is false, and no conservative dictionary refresh is pending.

This proof removes the later ownerless commit dirty-page flush fallback for the
bounded transaction shape. It does not claim the active rollback-history WAL
proof used by the autocommit path: `trx_t::write_serialisation_history()` may
still report ownerless write-history flush pages before the commit visibility
gate. The test records that remaining write-history path as the next
performance boundary.

Reset the proof on explicit transaction start, commit, rollback, implicit
transaction end, and internal rollback cleanup.

## Scope And Non-Goals

In scope:

- Prepared and direct statement execution through `libmylite`.
- Explicit transactions started with `START TRANSACTION` or `BEGIN`.
- Transactions whose local writes are all covered by the existing
  `INSERT ... VALUES` visible-fast predicate.

Out of scope:

- `INSERT ... SELECT`, `UPDATE`, `DELETE`, `REPLACE`, DDL, table-lock mode,
  foreign-key target inserts, savepoint-controlled transactions, and
  locking-read transactions.
- Transaction-scoped page-log append batching.
- Broader native redo/checkpoint reconciliation.
- External MariaDB/RQG stress.

## Compatibility Impact

SQL results do not change. This is a commit-publication optimization for a
subset of ownerless explicit transactions that already used MariaDB/InnoDB for
transaction execution and durability. Unsupported or unproven statement shapes
stay on the conservative flush path.

## Directory And Native Storage Impact

No directory layout, public API, or durable file format changes are introduced.
The change affects when a proven ownerless explicit transaction can publish
page-version visibility without forcing the later native dirty-page flush
fallback. InnoDB page-version WAL, checkpoint, write-history flushing,
native-support proof, and recovery files remain in the existing MyLite-owned
database directory.

## Binary Size, License, And Dependency Impact

The slice adds no dependency and changes only first-party policy/tracking code
plus tests and docs. Binary-size impact should be negligible.

## Test And Verification Plan

- Update the focused explicit-transaction SQL test so prepared inserts inside
  one explicit transaction expect:
  - `commit_visibility_fast > 0`;
  - `commit_visibility_flush == 0`;
  - `commit_visibility_flush_unproven_statement == 0`;
  - no publish failure and no missing published pages;
  - transaction pages publish at COMMIT and the remaining write-history flush
    path has no fallback rounds, while pre-commit undo native-support pages are
    still elided.
- Add or retain reopen checks on the same handle, ownerless reopen, and forced
  `.shm` native reopen.
- Add a savepoint-controlled explicit transaction case that writes, rolls back
  to the savepoint, commits, and verifies the commit remains on the
  conservative unproven path while only the pre-savepoint row persists.
- Run adjacent ownerless selectors that cover autocommit visible-fast insert,
  native-support WAL elision, history proof, and foreign-key fast-path blocking.
- Run the reduced production embedded performance probe with stats enabled and
  disabled to capture explicit-transaction commit visibility and throughput.
- Run production-build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Proven explicit prepared insert transactions use visible-fast commit
  publication instead of `flush_unproven_statement`.
- The slice does not claim active history-proof publication for explicit
  transactions; the write-history flush path remains measured.
- Mixed, savepoint-controlled, or unproven transaction shapes are not newly
  claimed.
- Existing autocommit visible-fast behavior is unchanged.
- Docs and compatibility matrix identify the narrowed scope and remaining
  planned gaps.

## Verification Results

Completed locally on 2026-06-17 with `build/php-embedded-prod` at `Release`:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `mylite_ownerless_cross_process_sql_test
  explicit-transaction-visible-fast-commit`
- alias selector:
  `mylite_ownerless_cross_process_sql_test
  explicit-transaction-undo-wal-elision`
- adjacent selectors:
  `single-owner-multi-row-insert-visible-fast-path`,
  `single-owner-native-support-page-wal-elision`,
  `single-owner-history-wal-proof`, and
  `insert-fk-fast-path-cache`
- stats-enabled reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=50 MYLITE_PERF_INSERT_ITERATIONS=80
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- stats-off reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=50 MYLITE_PERF_INSERT_ITERATIONS=1000
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`

The stats-enabled sample reported
`commit_visibility_fast_per_transaction=1.000`,
`commit_visibility_flush_per_transaction=0.000`,
`commit_visibility_unproven_per_transaction=0.000`,
`page_publish_transaction_image_published_per_transaction=3.000`,
`page_publish_transaction_buffer_published_per_transaction=9.000`,
`native_support_elided_undo_pages_per_insert=1.012`,
`native_support_published_undo_pages_per_insert=0.000`,
`write_history_ownerless_flush_pages_per_transaction=2.000`, and explicit
transaction throughput at `2433.67` ownerless ops/s versus `4315.04` ordinary
ops/s, ratio `0.5640`.

The stats-off 1000-row run reported explicit transaction throughput at
`2307.08` ownerless ops/s versus `2708.92` ordinary ops/s, ratio `0.8517`.

The follow-up ownerless explicit transaction history-proof slice replaces this
remaining write-history flush for the same proven prepared-insert transaction
shape by publishing the active rollback-segment and undo history pages. The
visible-fast proof here remains the gate that prevents savepoint-controlled,
mixed, DDL, locking-read, foreign-key target, and otherwise unproven explicit
transactions from entering that history proof.

## Risks And Unresolved Questions

- The first implementation intentionally disqualifies locking reads and all
  non-`INSERT ... VALUES` writes. That leaves performance on the table but keeps
  the proof easy to audit.
- Failed visible-fast writes inside an explicit transaction may still need a
  broader failure-path audit before this proof is widened beyond the focused
  prepared-insert workload.
- The transaction starts tracked here are `START TRANSACTION`/`BEGIN`; broader
  autocommit-off transaction management can be considered after focused proof
  and performance evidence.
