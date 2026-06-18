# Ownerless Sixteen-Row Visible-Fast Batch

## Problem Statement

The eight-row visible-fast append-batch slice proved that a bounded parser-
recognized `INSERT ... VALUES` row list can keep one page-log append session
for the full statement without changing page-version or native-history proof
semantics. A fresh production probe at current head showed the next bounded row
list has the same shape: a 16-row ownerless bulk statement already uses
visible-fast commit publication, but the current eight-row cap falls back to
per-mini-transaction append sessions and disables deferred latest-checkpoint
coalescing.

The pre-slice 16-row probe on 2026-06-18 used `160` rows, `10` SQL statements,
and ownerless page-publish stats. It reported `185` page-log appends,
`171` append-session begin/end calls, visible-fast commit at `1.000` per
statement, conservative flush at `0.000`, and deferred latest-checkpoint
coalescing at `0.000` per statement.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` parses pure
  `INSERT ... VALUES` row lists and rejects unsupported trailing tokens,
  upsert, `INSERT ... SELECT`, and non-row-list shapes.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` already separates visible-fast
  commit eligibility from append-batch eligibility. The 16-row probe proves
  visible-fast commit publication already succeeds above the old append-batch
  cap.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes ownerless page-version
  images at mini-transaction boundaries. Multi-row inserts can therefore create
  many append-session release points unless the MyLite statement policy defers
  release until the SQL statement reaches the page-visible hook.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_deferred_latest_checkpoint_coalescing_allowed()` ties
  latest-only checkpoint coalescing to append-batch eligibility, so raising the
  bounded cap also removes redundant latest-only checkpoint work for the
  16-row shape.

## Design

Raise `k_ownerless_append_batch_fast_path_max_insert_values_rows` from eight to
sixteen. Keep every existing eligibility guard:

- only pure `INSERT ... VALUES` row lists are eligible;
- foreign-key target tables remain conservative;
- explicit target column lists that can touch `AUTO_INCREMENT` remain
  conservative outside a single-owner epoch;
- upsert, `INSERT ... SELECT`, `RETURNING`, DDL, locking reads, broad DML, and
  unsupported statement tails remain outside the append-batch path;
- page-visible publication still releases a deferred append session before
  syncing the page-version WAL and publishing the visible LSN.

## Scope And Non-Goals

In scope:

- direct and prepared ownerless `INSERT ... VALUES` statements with one through
  sixteen parser-proven row constructors;
- page-log append-session lifetime and latest-only checkpoint coalescing for
  that bounded shape;
- focused SQL coverage for a 16-row visible-fast statement.

Out of scope:

- row lists above sixteen;
- group commit across statements or processes;
- broader DML/DDL batching;
- changing page-version WAL records, checkpoint records, native InnoDB page
  images, native history-proof publication, or redo/checkpoint recovery.

## Compatibility Impact

No SQL result, public C API, PHP/mysqli behavior, metadata format, native
storage format, wire protocol, or directory layout changes. The committed rows
become visible at the same logical commit boundary. The change only keeps
process-local append and checkpoint helpers batched for a larger bounded
statement.

## Database Directory And Native Storage Impact

No durable files or directory paths change. The ownerless page-version WAL,
checkpoint file, shared-memory segments, and InnoDB native files remain in the
existing database-directory layout. Native history-proof rollback-segment and
undo pages are still published.

## Build And Performance Impact

The code change is first-party MyLite statement policy and SQL test coverage.
It does not touch upstream-derived MariaDB source or add dependencies.

Expected 16-row probe signal:

- page-log append count and page-version count remain stable;
- append-session begin/end calls drop from per-mini-transaction volume toward
  one per SQL statement;
- deferred latest-checkpoint coalescing becomes active for the 16-row shape.

## Test And Verification Plan

- Extend `single-owner-multi-row-insert-visible-fast-path` to assert a 16-row
  visible-fast insert publishes page versions, keeps native history proof, uses
  exactly one append session, and coalesces latest-checkpoint updates.
- Build the production embedded ownerless SQL harness and performance probe.
- Run the focused ownerless selector directly and through CTest.
- Run adjacent production ownerless selectors for primitives, native-support
  page WAL elision, visible-fast multi-row inserts, and FK fast-path cache.
- Run a 16-row production performance probe before and after the cap change.
- Run hook/stress coverage for the focused selector, production-build guards,
  format checks, and `git diff --check`.

## Acceptance Criteria

- Sixteen-row pure ownerless `INSERT ... VALUES` statements use one page-log
  append session while preserving visible-fast commit publication.
- Page-version publication and native history WAL proof remain covered.
- Unsupported upsert remains conservative.
- Production probe evidence shows reduced 16-row append-session churn or this
  slice is not kept.

## Implementation Evidence

The implementation raises
`k_ownerless_append_batch_fast_path_max_insert_values_rows` from `8` to `16`.
The existing row-list parser and target-table guards still define eligibility.

`test_ownerless_single_owner_multi_row_insert_visible_fast_path()` now adds a
16-row `INSERT ... VALUES` case. The case verifies visible-fast commit, zero
conservative-flush reasons, zero ownerless history dirty-page flushes, native
history WAL proof publication, one page-log append session, deferred latest-
checkpoint coalescing, and durable row visibility before the selector continues
through unsupported upsert and reopen coverage.

Local production evidence on 2026-06-18:

- Pre-slice 16-row probe:
  `MYLITE_PERF_INSERT_ITERATIONS=160`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=16`, and ownerless page-publish
  stats enabled reported `185` page-log appends, `171` append-session begin
  and end calls for `10` SQL statements, `0` deferred latest-checkpoint
  coalesces, visible-fast commit at `1.000` per statement, conservative flush
  at `0.000`, and ownerless 16-row bulk throughput at `7223.47` rows/s.
- Post-slice 16-row probe with the same shape kept `185` page-log appends,
  reduced append-session begin/end calls to `10`, enabled `320` deferred
  latest-checkpoint coalesces (`32.000` per statement), kept visible-fast
  commit at `1.000` per statement and conservative flush at `0.000`, reduced
  page-write publish total from `5.024 ms` to `3.284 ms`, reduced page-write
  commit-log total from `9.482 ms` to `5.557 ms`, and reported ownerless
  16-row bulk throughput at `10655.43` rows/s. The ordinary comparison stayed
  essentially flat at `38054.79` versus `38136.76` rows/s.

## Risks And Follow-Up

The cap is still a lock-hold-time tradeoff. A previous unbounded row-list
attempt regressed bulk timing, so this slice deliberately stops at sixteen rows
and records current probe evidence. Larger row lists, broader DML batching,
native history-proof replacement, and native redo/checkpoint reconciliation
remain separate work.
