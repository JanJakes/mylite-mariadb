# Ownerless 8192 Row Visible-Fast Batch

## Problem

Ownerless visible-fast append batching currently admits pure `INSERT ... VALUES`
statements through `4096` parsed row constructors. A reduced production probe
over two `8192`-row statements showed that the adjacent larger row-list shape
still used visible-fast commit publication, but stayed outside append-session
batching, transaction-deferred page publication, and deferred latest-checkpoint
coalescing. That left the statement on the expensive per-mini-transaction page
publication path.

This slice raises the bounded row-list cap to `8192` rows and proves that
`8193` rows still stay outside append batching and deferred latest-checkpoint
coalescing.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` scans SQL text for parser-
  proven row constructors and returns no count for non-plain row-list shapes.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` gates append batching and deferred
  latest-checkpoint coalescing on the explicit row-list cap. Row lists above
  the cap can still use visible-fast commit publication, but they do not keep
  one page-log append session or coalesce latest-only checkpoint updates across
  the statement.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_single_owner_multi_row_insert_visible_fast_path()` contains
  the focused positive/negative boundary coverage for the cap.

## Design

- Raise the pure row-list append-batch cap from `4096` to `8192`.
- Replace the focused SQL selector's `4096`/`4097` boundary pair with an
  `8192`/`8193` boundary pair.
- Run a reduced production performance probe at `8192` rows per statement and
  record page-log append sessions, page versions, snapshot-boundary publication,
  checkpoint coalescing, and throughput counters.
- Update compatibility and ownerless concurrency docs.

## Non-Goals

- Unbounded row-list admission.
- Append batching for broader DML, DDL, `INSERT ... SELECT`, `ON DUPLICATE KEY`
  branches, or peer-present ownerless statements.
- Cross-process group commit or broad redo/checkpoint reconciliation.
- Claiming the ownerless concurrency objective is complete.

## Compatibility Impact

SQL-visible behavior is unchanged: successful inserts insert the same rows and
duplicate/error behavior remains on the existing MariaDB path. The change only
extends the internal ownerless single-owner visible-fast batching policy to one
larger parser-proven row-list boundary.

## Directory And Lifecycle Impact

No directory-layout changes. All durable state remains inside the MyLite
database directory through existing InnoDB files and ownerless page-version WAL.

## Native Storage Impact

No native InnoDB page, redo, undo, or checkpoint format changes. Existing
native row-level undo work remains the larger write-throughput target.

## Baseline Evidence

Before this slice, the `4096` cap was measured with:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=16384
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=8192
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed, but the two `8192` row statements stayed outside append batching:

- page-log append-session begin/end calls: `16387`/`16387`;
- page versions: `8268.000` per statement;
- page-log append calls: `8270.500` per statement;
- snapshot-boundary publications: `16458` total;
- native-support published pages: `39.000` per statement;
- visible-fast commits: `1.000` per statement;
- deferred latest-checkpoint coalesces: `0.000` per statement;
- ownerless `mysql_query()`: `732.644 ms/statement`;
- ownerless bulk throughput: `11128.17 rows/s`;
- ownerless/ordinary bulk rows ratio: `0.1124`;
- remaining ownerless-minus-ordinary undo-report MTR commit time:
  `151.211 ms/statement`.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the direct
  `single-owner-multi-row-insert-visible-fast-path` selector.
- Run adjacent ownerless selectors covering history proof, native-support page
  WAL elision, FK fast-path cache, and uncommitted-peer hiding.
- Run a reduced stats-enabled production performance probe with
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=8192`.
- Run production build guards, format, and diff checks.

## Acceptance Criteria

- An `8192` row pure ownerless insert uses one append session and coalesces
  latest-only checkpoint updates.
- An `8193` row pure ownerless insert stays outside append-batch and deferred
  latest-checkpoint coalescing while preserving visible-fast commit
  publication.
- Docs record before/after evidence and keep broader native row-level undo,
  redo/checkpoint reconciliation, and unbounded row-list admission as separate
  remaining work.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`

The post-change reduced production probe used the same environment as the
baseline:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=16384
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=8192
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed and reported two bulk statements with `8192` rows per statement:

- page-log append-session begin/end calls: `2`/`2`;
- page-log append-session append calls: `55`;
- snapshot-boundary publications: `0`;
- page versions: `2.000` per statement;
- page-log append calls: `27.500` per statement;
- native-support published pages: `2.000` per statement;
- visible-fast commits: `1.000` per statement;
- deferred latest-checkpoint coalesces: `256.500` per statement;
- ownerless `mysql_query()`: `130.506 ms/statement`;
- ownerless bulk throughput: `61240.24 rows/s`;
- ownerless/ordinary bulk rows ratio: `0.6278`;
- later-statement ownerless/ordinary rows ratio: `0.4145`;
- remaining ownerless-minus-ordinary undo-report MTR commit time:
  `40.868 ms/statement`.

Compared with the same pre-slice probe, append-session begin/end calls moved
from `16387`/`16387` to `2`/`2`, snapshot-boundary publications moved from
`16458` to `0`, page versions moved from `8268.000` to `2.000` per statement,
deferred latest-checkpoint coalesces moved from `0.000` to `256.500` per
statement, ownerless `mysql_query()` moved from `732.644` to
`130.506 ms/statement`, and ownerless bulk throughput moved from `11128.17` to
`61240.24 rows/s`.

## Risks

- Larger parser-proven row-list admission can increase append-lock hold time
  for single-owner bulk statements. This slice remains bounded to `8192` rows
  and keeps peer-present statements on the conservative immediate path.
