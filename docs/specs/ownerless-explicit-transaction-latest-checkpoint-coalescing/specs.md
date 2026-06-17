# Ownerless Explicit Transaction Latest Checkpoint Coalescing

## Problem

The explicit transaction performance probe still reports one raw-latest
checkpoint update for every ownerless mini-transaction inside a prepared
`INSERT ... VALUES` transaction. The earlier deferred-latest coalescing slice
covered implicit/autocommit visible-fast statements only because explicit
transactions publish their durable page-visible boundary later, at `COMMIT`.

That caution is still correct for transaction-wide batching. Holding the
page-log append session or checkpoint state open for an arbitrary application
transaction would serialize unrelated work for the full transaction lifetime.
This slice only extends the existing statement-local latest-only checkpoint
coalescing to explicit transaction statements that are currently covered by the
same conservative visible-fast commit proof.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB/InnoDB mini-transaction commit owns native dirty page mutation and
  page-LSN assignment in `mariadb/storage/innobase/mtr/mtr0mtr.cc`; MyLite
  cannot advance the durable page-visible ownerless recovery boundary before
  the corresponding page image is in the ownerless page-version WAL or native
  checkpoint proof.
- MyLite explicit transaction visibility proof is tracked in
  `packages/libmylite/src/database.cc` through
  `ownerless_transaction_visible_fast_commit_candidate`,
  `ownerless_transaction_visible_fast_commit_disqualified`,
  `update_ownerless_explicit_transaction_visible_fast_proof_before_sql()`, and
  `ownerless_transaction_commit_allows_visible_fast_path()`.
- Raw-latest checkpoint publication flows from
  `ownerless_innodb_redo_leave_hook()` to
  `ownerless_persist_redo_checkpoint()`. Page-visible publication flows through
  `ownerless_innodb_pages_visible_hook()`, syncs the ownerless page-version WAL,
  publishes visible redo state, and writes the durable latest/visible checkpoint.
- Rebuilt shared redo state seeds latest and visible independently from
  `read_concurrency_checkpoint_lsn()` and
  `mylite_ownerless_redo_state_initialize()`. Page-version replay uses
  checkpoint `visible_lsn`, so coalescing later non-durable latest-only file
  rewrites does not advance the durable recovery boundary.

## Scope And Non-Goals

In scope:

- Coalesce later non-durable latest-only checkpoint updates inside one
  append-batched `INSERT ... VALUES` statement that runs inside an explicit
  transaction and is still covered by the visible-fast commit proof.
- Preserve the first successful latest-only checkpoint update for each eligible
  statement.
- Keep final durable latest/visible checkpoint publication at the existing
  statement or `COMMIT` boundary.
- Keep the optimization disabled after savepoints, locking reads, unsupported
  writes, foreign-key target inserts, peer dictionary refresh, and unsafe
  ownerless fault hooks.
- Add focused SQL coverage for both the eligible explicit transaction path and
  the savepoint-disqualified path.

Out of scope:

- Transaction-wide page-log append sessions.
- Cross-process group commit or broad lazy checkpoint publication.
- Coalescing durable page-visible checkpoint writes.
- Changing checkpoint file formats or page-version WAL formats.
- Extending visible-fast explicit transaction proof to `INSERT ... SELECT`,
  `UPDATE`, `DELETE`, `REPLACE`, DDL, locking-read, savepoint-controlled, or
  foreign-key target transactions.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  active-reader pressure policy, or external MariaDB/RQG stress.

## Design

`OwnerlessStatementVisibleFastPathScope` keeps the same statement-local lifetime
for page-log append deferral and deferred-latest checkpoint state. The statement
setup now asks a named helper whether latest-only checkpoint coalescing is
allowed:

- non-explicit append-batched statements keep the existing behavior,
- explicit transaction statements must also have a current visible-fast commit
  candidate, no disqualification, and no conservative dictionary refresh
  requirement.

`update_ownerless_explicit_transaction_visible_fast_proof_before_sql()` already
runs before the statement scope is created, so an eligible explicit
`INSERT ... VALUES` statement can turn on the candidate proof before the helper
checks it. Once a savepoint, locking read, unsupported write, or failed write
disqualifies the transaction, later statements cannot enable coalescing.

`ownerless_persist_redo_checkpoint()` still preserves the first eligible
latest-only checkpoint update in each statement. Later eligible updates in the
same statement increment
`checkpoint_update_deferred_latest_coalesced` and skip the checkpoint file
rewrite. Durable page-visible updates keep the previous path.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, or wire-protocol behavior changes. The change is
internal to ownerless checkpoint metadata publication for a bounded subset of
already-supported explicit transaction writes.

## Directory And Lifecycle Impact

No new durable files or file formats. The additional state is transient
statement policy in process memory. The existing `mylite-concurrency.ckpt`
latest/visible records remain the durable checkpoint source.

## Native Storage Impact

No native InnoDB, MyISAM, Aria, redo, page, tablespace, or dictionary format
changes. Native commit, page-version WAL sync, page-visible publication, and
durable checkpoint ordering are unchanged.

## Build, Size, License, And Dependencies

No dependency or license impact. Binary-size impact is limited to a small policy
helper, test assertions, probe summary rows, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Extend `test_ownerless_explicit_transaction_undo_wal_elision` to assert that
  the proven explicit transaction path coalesces latest-only checkpoint updates.
- Reset database perf stats after a savepoint disqualifies the proof and assert
  that the following statement does not coalesce latest-only updates.
- Extend `mylite_embedded_performance_probe` compact explicit transaction
  summaries with per-insert and per-transaction coalescing counters.
- Run focused explicit transaction SQL coverage, checkpoint/redo crash
  selectors, a reduced production performance probe, hook-build crash
  selectors, ownerless stress selectors, production-build guards, formatting,
  and whitespace checks.

## Acceptance Criteria

- Eligible explicit transaction inserts increment
  `checkpoint_update_deferred_latest_coalesced`.
- Savepoint-disqualified explicit transaction statements leave that counter at
  zero after the disqualification point.
- Fast explicit transaction commit visibility, history-proof publication, and
  same-handle/ownerless/native reopen visibility remain covered.
- Unsafe hook builds keep crash tests on the previous latest-checkpoint behavior.
- No transaction-wide append session or durable checkpoint coalescing is added.

## Implementation Evidence

- `ownerless_statement_deferred_latest_checkpoint_coalescing_allowed()` enables
  coalescing for append-batched non-explicit statements and for explicit
  transaction statements only when the transaction has a current visible-fast
  commit candidate and is not disqualified.
- `test_ownerless_explicit_transaction_undo_wal_elision` now enables database
  perf stats around the proven prepared insert transaction and asserts
  `checkpoint_update_deferred_latest_coalesced` is positive in production
  builds. The same selector asserts zero under unsafe hook builds, where fault
  hooks intentionally keep previous latest-checkpoint behavior.
- The savepoint negative case resets database perf stats after the savepoint
  disqualifies the proof and asserts the following write/rollback/commit path
  does not coalesce deferred latest checkpoints.
- `mylite_embedded_performance_probe` emits compact explicit transaction
  summary rows for deferred latest checkpoint coalescing per insert and per
  transaction.

Verification:

- `cmake --build build/php-embedded-prod --target mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe -j$(nproc)`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test explicit-transaction-undo-wal-elision`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test sql-case test_ownerless_single_owner_multi_row_insert_visible_fast_path`
- Production checkpoint/redo selectors:
  `checkpoint-lsn-noop-elision`, `statement-checkpoint-scheduling`,
  `visible-checkpoint-crash`, `redo-written-crash`, `redo-latest-crash`, and
  `redo-latest-checkpoint-crash`.
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=40 MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4 MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  reported explicit transaction coalescing `40`, explicit coalescing
  `1.000` per insert, autocommit coalescing `2.000` per insert, and four-row
  bulk coalescing `8.000` per statement.
- `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=400 MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4 build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  reported ordinary explicit transaction throughput `4168.89 ops/s`,
  ownerless explicit transaction throughput `2880.92 ops/s`, and explicit
  transaction ratio `0.6911` in that local run.
- `cmake --build build/ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j$(nproc)`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test explicit-transaction-undo-wal-elision`
- The same checkpoint and redo crash selectors passed under
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test`.
- `cmake --build build/ownerless-stress --target mylite_ownerless_cross_process_sql_test -j$(nproc)`
- `ctest --test-dir build/ownerless-stress --output-on-failure -R 'libmylite\\.ownerless-(single-owner-multi-row-insert-visible-fast-path|cross-process-transaction-stress|cross-process-checksum-stress)$'`

## Risks And Follow-Up

- This is not a full explicit transaction performance fix. Remaining cost still
  includes native commit, page-version publication, history-proof publication,
  and page-log append work.
- A later transaction-wide batching design would need separate correctness and
  fairness proof so user transaction idle time does not hold global append or
  checkpoint resources.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  active-reader pressure policy, SQL-level table-lock fault injection, and
  external MariaDB/RQG stress remain completion gaps.
