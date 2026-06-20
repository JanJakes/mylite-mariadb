# Ownerless 2048 Row Visible-Fast Batch

## Problem

Ownerless page-log append batching is proven for pure `INSERT ... VALUES`
statements through 1024 parsed row constructors. The parser proof, append
session protocol, transaction-deferred page publication, and checkpoint
coalescing rules are not inherently tied to that number, but prior unbounded
batching regressed stress behavior. The next safe performance step is one
larger bounded shape with an adjacent conservative boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` counts row constructors for pure
  `INSERT ... VALUES` statements and enables append-batch and
  transaction-deferred page publication only when the count is inside the
  configured cap.
- The same function keeps visible-fast commit publication separate from
  append-batch/deferred page publication. Row lists above the cap can still use
  visible-fast commit publication, but they release page-log append sessions at
  each mini-transaction boundary and do not coalesce latest-only checkpoint
  updates across the statement.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_single_owner_multi_row_insert_visible_fast_path()` verifies
  the positive cap boundary and adjacent conservative boundary for the focused
  ownerless SQL selector.

## Design

Raise the append-batch fast-path cap from `1024` to `2048` row constructors.
Keep all other gates unchanged:

- statement must be parser-proven pure `INSERT ... VALUES`;
- target table must not have foreign keys;
- transaction-deferred page publication still requires a proven single-owner
  epoch;
- page-log append sessions still release before page-visible LSN publication;
- row lists above the cap remain conservative for append-batch and
  latest-checkpoint coalescing.

The focused selector replaces the 1024/1025 boundary pair with a 2048/2049
pair. That keeps the test bounded while proving the new edge.

## Compatibility Impact

No SQL syntax, public C API, PHP/mysqli behavior, wire-protocol behavior,
storage format, or directory layout changes. The same inserted rows become
visible and durable; only the ownerless append-session lifetime changes for a
larger proven row-list shape.

## Directory And Lifecycle Impact

No new durable files or shared-memory fields are introduced. Existing page-log
WAL, checkpoint, recovery, and cleanup paths are used.

## Native Storage Impact

No native InnoDB page, redo, undo, or checkpoint format changes. The same
history-proof and native-support page-version publications remain required.

## Build And Performance Impact

The implementation changes first-party MyLite policy and focused tests only.
It does not require a MariaDB embedded archive rebuild. The expected benefit is
limited to large pure ownerless `INSERT ... VALUES` statements between 1025 and
2048 rows, where append-session begin/end churn and latest-only checkpoint
updates can now be coalesced across the statement.

This does not address single-row history-proof/native-support publication
volume, process-isolated WordPress startup, redo/checkpoint reconciliation, or
unbounded row-list admission.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the focused
  `single-owner-multi-row-insert-visible-fast-path` selector.
- Run adjacent ownerless selectors covering history proof, native-support page
  publication, FK fast-path cache invalidation, and active-reader pressure.
- Run a reduced stats-enabled production probe with
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048` to verify one visible-fast
  commit per statement, one append-session begin/end per statement, deferred
  latest-checkpoint coalescing, and stable history/native proof publication.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(history-wal-proof|native-support-page-wal-elision|multi-row-insert-visible-fast-path)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  commit-race`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-reader-pressure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read`

The reduced stats-enabled production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=20480
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed and reported `10` bulk statements with `2048` rows per statement:

- append-session begin calls: `10`;
- append-session end calls: `10`;
- page-log append calls: `119`, or `11.900` per statement;
- snapshot-boundary publications: `0`;
- history-proof rollback-segment pages: `10`;
- history-proof undo pages: `10`;
- page versions: `2.000` per statement;
- native-support published pages: `2.000` per statement;
- native-support elided pages: `1846.900` per statement;
- commit-visibility fast path: `1.000` per statement;
- commit-visibility flush path: `0.000` per statement;
- deferred latest-checkpoint coalesces: `115.200` per statement.

This confirms the 2048-row edge uses one append session and keeps the existing
history/native proof shape. The same sample reported ownerless/ordinary bulk
rows ratio `0.3477`, with the first statement at `1.3253` and later statements
at `0.3195`. Throughput from this reduced probe is not treated as a stable
benchmark, but the attribution points the next write-performance slice at
remaining native row-insert/undo-report cost rather than append-session churn.

## Acceptance Criteria

- A 2048-row pure ownerless insert uses one append session and coalesces
  latest-only checkpoint updates.
- A 2049-row pure ownerless insert stays outside append-batch and deferred
  latest-checkpoint coalescing while preserving visible-fast commit
  publication.
- Focused history/native/active-reader coverage remains green.
- Docs continue to identify history-proof/native-support publication volume,
  broader DML/DDL, cross-statement group commit, and still larger row lists as
  remaining write-performance targets.

## Risks And Follow-Up

- Holding the append lock through a larger statement can increase contention
  for another writer. This slice remains bounded to 2048 rows and keeps peer
  present and unsupported statement shapes on the conservative path.
- Larger row-list caps, broader DML, and cross-process group commit need
  separate contention evidence.
