# Ownerless 4096 Row Visible-Fast Batch

## Problem Statement

Ownerless page-log append batching is proven for pure `INSERT ... VALUES`
statements through `2048` parsed row constructors. The same parser proof,
append-session protocol, transaction-deferred page publication, and
latest-checkpoint coalescing rules are not inherently tied to `2048`, but prior
unbounded row-list admission regressed stress behavior. The next bounded
performance slice is to move one larger edge onto the existing fast path and
keep the adjacent row count conservative.

This slice raises the bounded row-list cap to `4096` rows and proves that
`4097` rows still stay outside append batching and deferred latest-checkpoint
coalescing.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` scans SQL text for parser-
  proven pure `INSERT ... VALUES` row constructors.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` keeps visible-fast commit
  publication separate from append-batch and deferred-page-publish eligibility.
  Row lists above the cap can still use visible-fast commit publication, but
  they release page-log append sessions at each mini-transaction boundary and
  do not coalesce latest-only checkpoint updates across the statement.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_single_owner_multi_row_insert_visible_fast_path()` already
  proves the positive cap boundary and adjacent conservative boundary.

## Scope And Non-Goals

In scope:

- Raise the pure row-list append-batch cap from `2048` to `4096`.
- Replace the focused SQL selector's `2048`/`2049` boundary pair with a
  `4096`/`4097` boundary pair.
- Run a reduced production performance probe at `4096` rows per statement and
  record the append-session, page-version, native-support, commit-visibility,
  checkpoint-coalescing, and throughput counters.
- Update compatibility and ownerless concurrency docs.

Out of scope:

- Unbounded row-list append batching.
- Broader DML, `INSERT ... SELECT`, CTAS, upsert, duplicate-handling DML,
  foreign-key tables, secondary-index-heavy tables, explicit transactions, or
  peer-present ownerless statements.
- Cross-process group commit or broad redo/checkpoint reconciliation.
- Claiming the ownerless concurrency objective is complete.

## Design

`k_ownerless_append_batch_fast_path_max_insert_values_rows` moves from `2048`
to `4096`. All existing gates remain unchanged:

- statement must be parser-proven pure `INSERT ... VALUES`;
- target table must not have foreign keys;
- transaction-deferred page publication still requires a proven single-owner
  epoch;
- page-log append sessions still release before page-visible LSN publication;
- row lists above the cap remain conservative for append-batch,
  transaction-deferred page publication, and latest-checkpoint coalescing.

The focused selector keeps its earlier small-row coverage and updates only the
large positive/negative cap pair. The `4096` row case must use one append
session and publish through the existing transaction-deferred page path. The
`4097` row case must still use visible-fast commit publication, but it must
produce snapshot-boundary publication, more than one append session, and zero
deferred latest-checkpoint coalesces.

## Compatibility Impact

No SQL syntax, public C API, PHP/mysqli behavior, wire-protocol behavior,
storage format, or directory-layout change. The same rows are inserted and
become visible and durable. Only the internal ownerless append-session lifetime
changes for one larger proven pure row-list shape.

## Directory And Lifecycle Impact

No new durable files, shared-memory fields, or database-directory lifecycle
states are introduced. Existing page-version WAL, checkpoint, recovery, and
cleanup paths are used.

## Native Storage Impact

No native InnoDB page, redo, undo, or checkpoint format changes. Existing
history-proof and native-support proof publications remain required.

## Build, Size, License, And Dependencies

No dependency, license, binary-size, or production build-profile change. The
implementation changes first-party MyLite policy and focused tests only; it
does not require a MariaDB embedded archive rebuild.

## Baseline Evidence

Before this slice, the current `2048` cap was measured with:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=20 \
MYLITE_PERF_INSERT_ITERATIONS=8192 \
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4096 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed, but the two `4096` row statements stayed outside append batching:

- page-version records: `4136.000` per statement;
- page-log append calls: `4138.500` per statement;
- native-support published pages: `21.000` per statement;
- visible-fast commits: `1.000` per statement;
- conservative-flush commits: `0.000` per statement;
- deferred latest-checkpoint coalesces: `0.000` per statement;
- ownerless bulk throughput: `9587.08` rows/s;
- ownerless/ordinary bulk rows ratio: `0.0967`;
- ownerless `mysql_query()`: `424.537 ms/statement`.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the focused
  `single-owner-multi-row-insert-visible-fast-path` selector.
- Run adjacent ownerless selectors covering history proof, native-support page
  publication, FK fast-path cache invalidation, and uncommitted peer behavior.
- Run a reduced stats-enabled production probe with
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4096`.
- Run `tools/check-ci-production-builds`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced stats-enabled production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=8192
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4096
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed and reported two bulk statements with `4096` rows per statement:

- append-session begin calls: `2`;
- append-session append calls: `35`;
- append-session end calls: `2`;
- page-version records: `2.000` per statement;
- page-log append calls: `17.500` per statement;
- native-support published pages: `2.000` per statement;
- native-support elided pages: `2055.500` per statement;
- visible-fast commits: `1.000` per statement;
- conservative-flush commits: `0.000` per statement;
- deferred latest-checkpoint coalesces: `128.000` per statement;
- ownerless `mysql_query()`: `66.720 ms/statement`;
- ownerless bulk throughput: `58904.02` rows/s;
- ownerless/ordinary bulk rows ratio: `0.6229`;
- later-statement ownerless/ordinary rows ratio: `0.3765`;
- remaining ownerless undo-report MTR commit time: `23.913 ms/statement`.

Compared to the same reduced pre-slice probe, append-session begin/end calls
fell from `8195`/`8195` to `2`/`2`, page-version records fell from `4136.000`
to `2.000` per statement, page-log append calls fell from `4138.500` to
`17.500` per statement, native-support published pages fell from `21.000` to
`2.000` per statement, deferred latest-checkpoint coalesces rose from `0.000`
to `128.000` per statement, ownerless `mysql_query()` moved from `424.537` to
`66.720 ms/statement`, and ownerless bulk throughput moved from `9587.08` to
`58904.02` rows/s. Throughput from the reduced probe is treated as directional
evidence only; the counter changes prove the intended policy transition.

## Acceptance Criteria

- A `4096` row pure ownerless insert uses one append session and coalesces
  latest-only checkpoint updates.
- A `4097` row pure ownerless insert stays outside append-batch and deferred
  latest-checkpoint coalescing while preserving visible-fast commit
  publication.
- Focused history/native/FK/peer-adjacent coverage remains green.
- Docs continue to identify broader DML/DDL, cross-process group commit,
  redo/checkpoint reconciliation, native row-insert/undo cost, and unbounded
  row-list admission as separate remaining work.

## Risks And Follow-Up

- Holding the append lock through a larger single statement can increase
  contention for another writer. This slice remains bounded to `4096` rows and
  keeps peer-present and unsupported statement shapes on the conservative path.
- Larger row-list caps, broader SQL shapes, cross-process group commit, and
  native row-level undo costs need separate evidence.
