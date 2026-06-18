# Ownerless Eight-Row Visible-Fast Batch

## Problem

The default production bulk insert probe uses four-row `INSERT ... VALUES`
statements, and the existing append-batch cap covers that shape. A follow-up
eight-row production diagnostic showed a bounded larger row list still qualifies
for ownerless visible-fast commit publication, but the four-row append-batch cap
forces the statement back to per-mini-transaction page-log append sessions and
disables deferred latest-checkpoint coalescing.

The measured eight-row sample for `80` rows reported `10` SQL statements,
`105` page-log appends, but `91` append-session begin/end calls. The same shape
reported zero deferred latest-checkpoint coalescing even though commit
visibility stayed on the fast path. That is avoidable fixed work for a bounded
parser-proven row list.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` opens and closes MyLite
  page-publish batches around ownerless dirty-page publication in mini-
  transaction commit paths. Multi-row inserts can therefore generate several
  append-session release points before the SQL statement publishes visibility.
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` parses pure
  `INSERT ... VALUES` row lists and rejects unsupported trailing tokens, upsert,
  and `INSERT ... SELECT` shapes.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` already separates visible-fast
  commit eligibility from the narrower append-batch eligibility. Before this
  slice, pure insert row lists above four still used visible-fast commit but did
  not defer page-log append-session release.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_deferred_latest_checkpoint_coalescing_allowed()` ties
  deferred latest-checkpoint coalescing to append-batch eligibility. Raising the
  bounded cap therefore also removes redundant latest-only checkpoint writes for
  the eight-row shape.

## Design

Raise the parser-proven append-batch cap from four to eight row constructors for
eligible ownerless `INSERT ... VALUES` statements. The same existing guards
remain in place:

- the target table must not have foreign keys;
- explicit target column lists that can touch `AUTO_INCREMENT` remain
  conservative outside a single-owner epoch;
- unsupported statement tails, upserts, `INSERT ... SELECT`, `RETURNING`, DDL,
  and broad DML stay outside the append-batch path;
- the page-visible hook still releases any deferred append session before
  syncing the page-version WAL and publishing the visible LSN.

## Scope And Non-Goals

In scope:

- direct and prepared ownerless `INSERT ... VALUES` statements with one through
  eight parser-proven row constructors;
- page-log append-session lifetime and deferred latest-checkpoint coalescing;
- focused SQL coverage for an eight-row visible-fast statement.

Out of scope:

- row lists above eight;
- broader DML, DDL, upsert, `INSERT ... SELECT`, `RETURNING`, foreign-key target
  tables, or group commit across statements or processes;
- changing page-version WAL records, checkpoint records, native InnoDB page
  images, or history-proof publication.

## Compatibility Impact

No SQL result, public C API, PHP/mysqli behavior, metadata format, native
storage format, or directory layout changes. The committed rows are visible at
the same logical commit boundary; the change only keeps process-local append
and checkpoint helpers batched for a larger bounded statement.

## Directory And Native Storage Impact

No new durable files or directory paths are introduced. The ownerless page-
version WAL, checkpoint file, shared-memory segments, and InnoDB native files
remain in the existing database-directory layout.

## Build And Performance Impact

The code change is first-party MyLite statement policy and SQL test coverage.
It does not touch upstream-derived MariaDB source or add dependencies.

Expected eight-row probe signal:

- page-log append count and page-version record count remain stable;
- append-session begin/end calls drop from per-mini-transaction volume toward
  one per SQL statement;
- deferred latest-checkpoint coalescing becomes active for the eight-row shape.

## Test And Verification Plan

- Extend `single-owner-multi-row-insert-visible-fast-path` to assert an eight-
  row visible-fast insert publishes page versions, keeps native history proof,
  uses exactly one append session, and coalesces latest-checkpoint updates.
- Run the focused ownerless selector under the production embedded build.
- Run the adjacent primitive/native-support/FK fast-path selectors.
- Run production performance probes for the eight-row bulk shape and compare
  append-session and coalescing counters before and after the cap change.
- Run production build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- Eight-row pure ownerless `INSERT ... VALUES` statements use one page-log
  append session while preserving visible-fast commit publication.
- Page-version publication and native history WAL proof remain covered.
- Unsupported upsert remains conservative.
- Production probe evidence shows reduced eight-row append-session churn or the
  docs record why the candidate did not improve the measured path.

## Implementation Evidence

The implementation raises
`k_ownerless_append_batch_fast_path_max_insert_values_rows` from `4` to `8`.
The existing row-list parser and target-table guards still define eligibility.

`test_ownerless_single_owner_multi_row_insert_visible_fast_path()` now adds an
eight-row `INSERT ... VALUES` case. The case verifies visible-fast commit,
zero conservative-flush reasons, zero ownerless history dirty-page flushes,
native history WAL proof publication, one page-log append session, deferred
latest-checkpoint coalescing, and durable row visibility before later reopening
coverage in the same selector.

Local production verification on 2026-06-18:

- `cmake --build build/php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- Direct `mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path` passed.
- `ctest --test-dir build/php-embedded-prod --output-on-failure -R
  '^libmylite\.ownerless-single-owner-multi-row-insert-visible-fast-path$'`
  passed.
- Adjacent embedded selectors passed for ownerless primitives, native-support
  page WAL elision, multi-row insert visible fast path, and insert foreign-key
  fast-path cache.
- The same adjacent selector subset passed under `ownerless-test-hooks`.
- The `ownerless-stress` build passed
  `libmylite.ownerless-single-owner-multi-row-insert-visible-fast-path`.
- The post-change eight-row production probe with
  `MYLITE_PERF_INSERT_ITERATIONS=80`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=8`, and ownerless publish stats
  enabled reported `105` page-log appends, `10` append-session begin/end calls
  for `10` SQL statements, `10.500` append calls per statement, `10.000` page
  versions per statement, visible-fast commit at `1.000` per statement,
  conservative flush at `0.000`, and deferred latest-checkpoint coalescing at
  `16.000` per statement. The pre-slice eight-row sample on the same branch
  reported `105` appends, `91` append-session begin/end calls, and `0.000`
  deferred latest-checkpoint coalesces per statement.
- `tools/check-ci-production-builds`, `cmake --build --preset
  format-check-prod`, and `git diff --check` passed.

## Risks And Follow-Up

The cap is still a lock-hold-time tradeoff. A previous unbounded row-list
attempt regressed bulk timing, so this slice deliberately stops at eight rows.
Larger row lists, broader DML batching, native history-proof replacement, and
native commit/write-history reduction require separate evidence.
