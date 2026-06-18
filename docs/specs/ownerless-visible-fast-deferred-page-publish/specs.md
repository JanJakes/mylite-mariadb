# Ownerless Visible-Fast Deferred Page Publish

## Problem Statement

After the streamed row-count slice made the 256-row visible-fast append-batch
boundary explicit, the 256-row production probe still showed high ownerless
write-path overhead. Append-session churn was already fixed, but page images
were still published at mini-transaction cadence:

- `2609` page-log append calls for `10` SQL statements;
- `5164` page-write publish calls;
- `60.789 ms` in page-write publish;
- `61.565 ms` in commit-log publish attribution;
- `36.837 ms` in page-log append;
- ownerless 256-row bulk throughput at `12178.41` rows/s.

The existing transaction-deferred page publication path already captures the
latest image per `(space_id, page_no)` and proves commit visibility through
`mylite_ownerless_innodb_publish_transaction_pages_to_lsn()`. Bounded
visible-fast autocommit row-list statements can use the same proof at the SQL
statement boundary instead of publishing every intermediate page image.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc`
  `OwnerlessStatementVisibleFastPathScope` already brackets direct and
  prepared execution and knows whether the statement is eligible for page-log
  append batching.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_uses_transaction_release()` previously rejected
  visible-fast autocommit statements, so each ownerless mini-transaction
  published page images immediately.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_capture_dirty_transaction_page()` stores one current
  image per `(space_id, page_no)` and replaces older captured images when a
  later page LSN is observed.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` publishes
  captured transaction images first, falls back to buffer-pool publication for
  tracked pages when needed, and sets
  `mylite_ownerless_page_write_deferred_pages_published` only when the deferred
  dirty-page proof is complete.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  commit visibility already treats successfully proven transaction-deferred
  pages as eligible for the visible-fast path and otherwise falls back to the
  conservative dirty-page flush bridge.

## Design

Add a thread-local InnoDB statement flag,
`mylite_ownerless_statement_deferred_page_publish`, with public hook helpers:

- `mylite_ownerless_innodb_set_statement_deferred_page_publish()`;
- `mylite_ownerless_innodb_statement_deferred_page_publish()`.

`OwnerlessStatementVisibleFastPathScope` sets that flag only when the existing
MyLite SQL policy already allows page-log append batching. That keeps all
current eligibility guards:

- pure `INSERT ... VALUES` row lists only;
- one through 256 streaming-proven row constructors;
- no foreign-key target tables;
- explicit target column lists that can touch `AUTO_INCREMENT` remain
  conservative outside a single-owner epoch;
- row lists above 256, upsert, `INSERT ... SELECT`, `RETURNING`, broader DML,
  DDL, and locking reads do not use this path.

When the flag is set, `ownerless_page_write_uses_transaction_release()` allows
visible-fast autocommit MTRs to use the existing transaction-deferred page
publish machinery. Commit still calls
`mylite_ownerless_innodb_publish_transaction_pages_to_lsn()` before publishing
the page-visible LSN. If captured images or buffer-pool fallback cannot prove
the deferred pages, the existing conservative flush counters and path remain
active.

## Scope And Non-Goals

In scope:

- bounded visible-fast append-batched `INSERT ... VALUES` statements;
- statement-scoped page-image coalescing through existing transaction-deferred
  publication;
- focused 256-row positive and 257-row conservative boundary coverage;
- production probe evidence for the 256-row bulk shape.

Out of scope:

- row lists above 256;
- broad DML/DDL deferred publication;
- group commit across statements or processes;
- replacing rollback-segment/undo native history proof;
- changing page-version WAL format, checkpoint format, or native InnoDB page
  images.

## Compatibility Impact

No SQL result, public C API, PHP/mysqli behavior, metadata format, native
storage format, wire protocol, or directory layout changes. Committed rows
become visible at the same logical statement commit boundary.

The change reduces intermediate ownerless page-version WAL records for a
bounded statement. Snapshot readers still see the pre-statement version until
the statement's page-visible LSN is published, matching the existing
visible-fast autocommit semantics.

## Database Directory And Native Storage Impact

No durable file names or directories change. The ownerless page-version WAL and
checkpoint files remain in the existing database-directory layout. The slice
only changes which already-supported page images are appended to the WAL for a
bounded statement.

## Build And Performance Impact

The change touches first-party MyLite statement scope code and MyLite-owned
InnoDB hook code in the MariaDB fork. It adds no dependencies and does not
change the embedded profile.

Expected 256-row probe signal:

- page-log append calls drop from per-row volume toward the distinct final page
  image set plus native history proof pages;
- page-write publish and commit-log publish attribution drop substantially;
- commit visibility remains fast with zero conservative flush;
- 257-row statements remain outside append batching and keep per-row page-log
  append volume.

## Test And Verification Plan

- Extend `single-owner-multi-row-insert-visible-fast-path`:
  - 256-row case verifies fast commit, no conservative flush, no publish
    failure, no snapshot-boundary immediate user-page publication, positive
    transaction image/buffer publication, one page-log append session, deferred
    checkpoint coalescing, and durable row visibility.
  - 257-row case verifies fast commit, positive snapshot-boundary immediate
    publication, more than one append session, zero deferred checkpoint
    coalescing, and durable row visibility.
- Build the MariaDB embedded archive because this slice touches InnoDB hook
  sources.
- Build production ownerless SQL and performance probe targets.
- Run the focused selector directly and through production CTest.
- Run adjacent production ownerless selectors for primitives, native-support
  page WAL elision, visible-fast multi-row inserts, and FK fast-path cache.
- Run hook/stress focused selectors, production-build guards, format checks,
  and `git diff --check`.
- Run 256-row production performance probes before and after the change.

## Acceptance Criteria

- Bounded visible-fast row-list statements preserve fast commit publication and
  durable visibility before and after ownerless/native reopen.
- The 256-row focused case proves transaction-deferred publication is used.
- The 257-row focused case proves the cap still keeps larger row lists outside
  append batching and deferred checkpoint coalescing.
- Production probe evidence shows reduced 256-row page-log append volume and
  ownerless bulk throughput improvement.

## Implementation Evidence

The implementation adds a statement-local InnoDB hook flag:

- `mylite_ownerless_innodb_set_statement_deferred_page_publish()`;
- `mylite_ownerless_innodb_statement_deferred_page_publish()`.

`OwnerlessStatementVisibleFastPathScope` sets the flag only when
`fast_path_policy.append_batch_fast_path` is true. InnoDB
`ownerless_page_write_uses_transaction_release()` then allows visible-fast
autocommit MTRs to use transaction-deferred page-write release only under that
statement flag.

`test_ownerless_single_owner_multi_row_insert_visible_fast_path()` now verifies
the 256-row bounded path has positive transaction image/buffer publication and
zero snapshot-boundary immediate user-page publication. The same selector also
verifies the 257-row guard still uses immediate snapshot-boundary publication,
opens more than one append session, and does not coalesce latest-only
checkpoint updates.

Local production evidence on 2026-06-18:

- Pre-slice 256-row probe after the streamed row-count slice:
  `MYLITE_PERF_INSERT_ITERATIONS=2560`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=256`, and ownerless
  page-publish stats enabled reported `2609` page-log append calls, `10`
  append-session begin/end calls, `5164` page-write publish calls,
  `60.789 ms` page-write publish total, `61.565 ms` commit-log publish
  attribution, `36.837 ms` page-log append total, visible-fast commit at
  `1.000` per statement, conservative flush at `0.000`, and ownerless bulk
  throughput at `12178.41` rows/s.
- Post-slice 256-row probe with the same shape reported `68` page-log append
  calls, `10` append-session begin/end calls, `2580` page-write publish calls,
  `2.938 ms` page-write publish total, `6.767 ms` commit-log publish
  attribution, `3.607 ms` page-log append total, `36` transaction image
  publications, `12` transaction buffer publications, zero snapshot-boundary
  immediate user-page publications, visible-fast commit at `1.000` per
  statement, conservative flush at `0.000`, and ownerless bulk throughput at
  `20008.38` rows/s.
- Post-slice 128-row probe reported `50` page-log append calls, `10`
  append-session begin/end calls, `2.486 ms` page-write publish total,
  `4.015 ms` commit-log publish attribution, `3.214 ms` page-log append total,
  `24` transaction image publications, `6` transaction buffer publications,
  zero snapshot-boundary immediate user-page publications, visible-fast commit
  at `1.000` per statement, conservative flush at `0.000`, and ownerless bulk
  throughput at `21337.78` rows/s.
- Post-slice 257-row guard probe remained outside the bounded path: it reported
  `2619` page-log append calls, `2581` append-session begin/end calls, `2582`
  snapshot-boundary page publications for `10` statements, visible-fast commit
  at `1.000` per statement, conservative flush at `0.000`, and ownerless bulk
  throughput at `11537.43` rows/s.

## Risks And Follow-Up

This holds page-write ownership and captured page images until statement
commit for the bounded path. The 256-row cap remains a lock-hold-time guard.
Broader DML, DDL, unbounded row lists, cross-process group commit, and native
history-proof replacement remain separate work.
