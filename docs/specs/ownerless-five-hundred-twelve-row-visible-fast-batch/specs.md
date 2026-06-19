# Ownerless Five Hundred Twelve Row Visible-Fast Batch

## Problem Statement

Ownerless visible-fast append batching currently covers pure
`INSERT ... VALUES` statements with one through `256` streaming-proven row
constructors. The production performance probe shows that larger pure row-list
statements still use fast commit visibility, but row lists above the cap fall
back to per-mini-transaction append-session and checkpoint-update work.

This slice raises the bounded row-list cap to `512` rows after the existing
streaming SQL-text counter and statement-deferred page-publish proof made the
row-list boundary explicit. It keeps the cap finite and proves `513` rows stay
outside the append-batch and deferred-page-publish path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` scans the original SQL text
  with `next_sql_token()`, so eligibility is no longer limited by
  `k_sql_policy_token_count`.
- `packages/libmylite/src/database.cc`
  `k_ownerless_append_batch_fast_path_max_insert_values_rows` is the explicit
  cap for page-log append batching and statement-deferred page publication.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` still requires a pure row-list
  insert, rejects foreign-key target tables, and enables deferred page publish
  only while the process registry proves a single-owner epoch.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` and
  `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc` already
  provide the transaction-deferred page capture/publish proof used by the
  existing `256` row path.

## Scope And Non-Goals

In scope:

- Raise the bounded pure-row-list append-batch cap from `256` to `512`.
- Prove a `512` row ownerless `INSERT ... VALUES` statement uses fast commit
  visibility, one page-log append session, deferred latest checkpoint
  coalescing, and transaction-deferred page publication.
- Prove a `513` row ownerless `INSERT ... VALUES` statement remains visible-fast
  but outside append batching and deferred latest checkpoint coalescing.
- Record a reduced production performance probe before and after the cap
  change for the `512` row shape.

Out of scope:

- Unbounded row-list append batching.
- Broad DML/DDL batching, `INSERT ... SELECT`, upsert, `RETURNING`, locking
  reads, or foreign-key target tables.
- Group commit across statements or processes.
- Changing page-version WAL format, checkpoint format, native InnoDB page
  images, native history-proof publication, or redo/checkpoint recovery.

## Design

The implementation changes only the first-party MyLite cap:

```c++
constexpr std::size_t k_ownerless_append_batch_fast_path_max_insert_values_rows = 512U;
```

The existing row-list parser, visible-fast eligibility checks,
single-owner-epoch requirement for deferred page publication, native
history-proof page publication, WAL sync ordering, and page-visible LSN
publication remain unchanged.

`test_ownerless_single_owner_multi_row_insert_visible_fast_path()` moves the
positive boundary case from `256` to `512` and the conservative guard from
`257` to `513`. The test still verifies:

- fast commit visibility and zero conservative flush;
- no ownerless write-history flush;
- native history-proof rollback-segment and undo page publication;
- one append session for the capped positive case;
- deferred latest checkpoint coalescing for the capped positive case;
- immediate snapshot-boundary publication and no deferred latest checkpoint
  coalescing for the first row count above the cap;
- durable row counts and sums.

## Compatibility Impact

No SQL result, public C API, PHP/mysqli behavior, metadata format, native
storage format, wire protocol, or directory layout changes. Eligible commits
become visible at the same logical statement boundary.

Pure `512` row inserts now use the same performance path already used by `256`
row inserts. Pure `513` row inserts remain conservative for append batching and
deferred checkpoint coalescing.

## Directory And Lifecycle Impact

No durable file names or directory paths change. The ownerless page-version WAL,
checkpoint file, shared-memory segments, and native InnoDB files keep the same
layout and durability rules. The slice changes only how many bounded row
constructors can share the existing statement-scoped append session and
deferred page publication proof.

## Build, Size, License, And Dependencies

No production build-profile, binary-size, license, or dependency changes. The
slice touches first-party MyLite policy/test code only.

## Verification Plan

- Capture a reduced `512` row production performance probe before the cap
  change.
- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the focused ownerless SQL selector
  `single-owner-multi-row-insert-visible-fast-path`.
- Run the matching production CTest selector.
- Run a reduced `512` row production performance probe after the cap change.
- Run adjacent ownerless SQL selectors that cover the same hot-path proof:
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`, and
  `insert-fk-fast-path-cache`.
- Run production-build guard, format check, and `git diff --check`.

## Verification Results

- Pre-slice reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=25`,
  `MYLITE_PERF_INSERT_ITERATIONS=5120`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=512`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported `10` bulk statements,
  ownerless bulk throughput `11181.55` rows/s, visible-fast commit at `1.000`
  per statement, conservative flush at `0.000`, `5131` append-session
  begin/end calls, `5193` page-log append calls, `5144` snapshot-boundary page
  publications, and zero deferred latest-checkpoint coalescing.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-multi-row-insert-visible-fast-path$'
  --output-on-failure` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)|insert-fk-fast-path-cache)$'
  --output-on-failure` passed.
- Post-slice reduced production probe with the same `512` row shape reported
  `10` bulk statements, ownerless bulk throughput `25810.38` rows/s,
  visible-fast commit at `1.000` per statement, conservative flush at `0.000`,
  `10` append-session begin/end calls, `90` page-log append calls, zero
  snapshot-boundary page publications, `1024.000` deferred latest-checkpoint
  coalesces per statement, `2.000` page-version records per statement, and
  `9.000` page-log append calls per statement.

## Acceptance Criteria

- `512` row pure ownerless `INSERT ... VALUES` statements use one page-log
  append session and deferred latest checkpoint coalescing.
- `513` row pure ownerless `INSERT ... VALUES` statements remain outside the
  append-batch and deferred checkpoint coalescing path.
- Fast commit publication, native history-proof publication, durable row
  visibility, ownerless reopen, and forced `.shm` rebuild behavior remain
  covered by existing adjacent selectors.
- Documentation keeps larger row lists, broader DML/DDL, group commit, and
  redo/checkpoint reconciliation as separate work.

## Risks And Follow-Up

- A `512` row cap is still a bounded append-lock and statement-deferred
  publication tradeoff, not an unbounded policy.
- Larger row lists need separate measurement before admission.
- Broad DML/DDL batching, native history-proof replacement, group commit, and
  native redo/checkpoint reconciliation remain separate work.
