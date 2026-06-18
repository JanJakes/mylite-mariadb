# Ownerless Thirty-Two-Row Visible-Fast Batch

## Problem Statement

The bounded eight-row and sixteen-row visible-fast append-batch slices showed
that parser-proven `INSERT ... VALUES` row lists can keep one page-log append
session for the full SQL statement while preserving page-version publication,
page-visible LSN ordering, and native history WAL proof. A fresh 32-row
production probe at current head showed the same fixed-work gap at the next
bounded row-list shape: visible-fast commit publication already succeeds, but
the sixteen-row append-batch cap falls back to per-mini-transaction append
sessions and disables deferred latest-checkpoint coalescing.

The pre-slice 32-row probe on 2026-06-18 used `320` rows, `10` SQL statements,
and ownerless page-publish stats. It reported `345` page-log appends,
`331` append-session begin/end calls, visible-fast commit at `1.000` per
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
  `ownerless_statement_fast_path_policy()` separates visible-fast commit
  eligibility from append-batch eligibility. The 32-row baseline proves
  visible-fast commit publication already succeeds above the old append-batch
  cap.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes ownerless page-version
  images at mini-transaction boundaries. Larger multi-row inserts can therefore
  create many append-session release points unless the MyLite statement policy
  defers release until the SQL statement reaches the page-visible hook.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_deferred_latest_checkpoint_coalescing_allowed()` ties
  latest-only checkpoint coalescing to append-batch eligibility, so raising the
  bounded cap also removes redundant latest-only checkpoint work for the
  32-row shape.

## Design

Raise `k_ownerless_append_batch_fast_path_max_insert_values_rows` from sixteen
to thirty-two. Keep every existing eligibility guard:

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
  thirty-two parser-proven row constructors;
- page-log append-session lifetime and latest-only checkpoint coalescing for
  that bounded shape;
- focused SQL coverage for a 32-row visible-fast statement.

Out of scope:

- row lists above thirty-two;
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

Expected 32-row probe signal:

- page-log append count and page-version count remain stable;
- append-session begin/end calls drop from per-mini-transaction volume toward
  one per SQL statement;
- deferred latest-checkpoint coalescing becomes active for the 32-row shape.

## Test And Verification Plan

- Extend `single-owner-multi-row-insert-visible-fast-path` to assert a 32-row
  visible-fast insert publishes page versions, keeps native history proof, uses
  exactly one append session, and coalesces latest-checkpoint updates.
- Build the production embedded ownerless SQL harness and performance probe.
- Run the focused ownerless selector directly and through CTest.
- Run adjacent production ownerless selectors for primitives, native-support
  page WAL elision, visible-fast multi-row inserts, and FK fast-path cache.
- Run a 32-row production performance probe before and after the cap change.
- Run hook/stress coverage for the focused selector, production-build guards,
  format checks, and `git diff --check`.

## Acceptance Criteria

- Thirty-two-row pure ownerless `INSERT ... VALUES` statements use one
  page-log append session while preserving visible-fast commit publication.
- Page-version publication and native history WAL proof remain covered.
- Unsupported upsert remains conservative.
- Production probe evidence shows reduced 32-row append-session churn or this
  slice is not kept.

## Implementation Evidence

The implementation raises
`k_ownerless_append_batch_fast_path_max_insert_values_rows` from `16` to `32`.
The existing row-list parser, policy token budget, and target-table guards
still define eligibility.

`test_ownerless_single_owner_multi_row_insert_visible_fast_path()` now adds a
32-row `INSERT ... VALUES` case using the same `(id, value)` row-list shape as
the embedded performance probe. The case verifies visible-fast commit, zero
conservative-flush reasons, zero ownerless history dirty-page flushes, native
history WAL proof publication, one page-log append session, deferred latest-
checkpoint coalescing, and durable row visibility before the selector continues
through unsupported upsert and reopen coverage. A first attempt with 32
`REPEAT()` payload expressions exceeded the fixed SQL policy token budget and
correctly stayed conservative, so the final focused case keeps the
parser-proven shape that the production probe measures.

Local production evidence on 2026-06-18:

- Pre-slice 32-row probe:
  `MYLITE_PERF_INSERT_ITERATIONS=320`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=32`, and ownerless page-publish
  stats enabled reported `345` page-log appends, `331` append-session begin
  and end calls for `10` SQL statements, `0` deferred latest-checkpoint
  coalesces, visible-fast commit at `1.000` per statement, conservative flush
  at `0.000`, and ownerless 32-row bulk throughput at `8345.99` rows/s.
- Post-slice 32-row probe with the same shape kept `345` page-log appends,
  reduced append-session begin/end calls to `10`, enabled `640` deferred
  latest-checkpoint coalesces (`64.000` per statement), kept visible-fast
  commit at `1.000` per statement and conservative flush at `0.000`, reduced
  page-write publish total from `9.274 ms` to `8.469 ms`, reduced page-write
  commit-log total from `18.504 ms` to `14.010 ms`, and reported ownerless
  32-row bulk throughput at `9307.03` rows/s. The ordinary comparison moved
  from `65202.61` to `61966.89` rows/s, so the ownerless ratio moved from
  `0.1280` to `0.1502`.

## Risks And Follow-Up

The cap is still a lock-hold-time tradeoff. A previous unbounded row-list
attempt regressed bulk timing, so this slice deliberately stops at thirty-two
rows and must record current pre/post probe evidence. Larger row lists,
broader DML batching, native history-proof replacement, and native
redo/checkpoint reconciliation remain separate work.
