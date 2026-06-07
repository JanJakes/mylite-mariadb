# Ownerless Prepared Policy Token Cache

## Problem

The ownerless SQL token reuse slice removed repeated tokenization inside one
statement execution, but ownerless prepared statements still rebuilt
`SqlPolicyTokens` on every `mylite_step()`. The embedded performance probe
shows ownerless prepared loops are still measurably behind ordinary prepared
loops, and prepared write loops also pay the larger conservative page-visible
flush bridge. Re-tokenizing the same prepared SQL text is avoidable CPU work on
top of that bridge.

This slice does not change the default ownerless write durability or dirty-page
flush policy. Local profiling confirmed ownerless autocommit insert throughput
stays low across `FULL`, `NORMAL`, and `OFF` durability because the dominant
cost is the ownerless page-version publication plus dirty-page flush bridge,
not the MariaDB redo flush setting.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:mylite_prepare()` retains prepared SQL
  text in `mylite_stmt::ownerless_sql_text` for ownerless statements.
- `packages/libmylite/src/database.cc:mylite_step()` used that retained text
  to call `collect_sql_policy_tokens()` on every execution before pressure,
  temporary-table, lock, page-version read, dictionary, transaction-state, and
  statement-checkpoint decisions.
- `SqlPolicyTokens` stores string views into the SQL text. A prepared
  statement's SQL text is immutable until `mylite_finalize()`, so one cached
  token set can safely live with the statement.
- `mariadb/storage/innobase/trx/trx0trx.cc` and
  `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc` keep
  ownerless page-version publication and dirty-page flush behavior unchanged.

## Design

- Add cached ownerless policy tokens to `mylite_stmt`.
- Populate the cache once during ownerless `mylite_prepare()` after the SQL
  text is copied into statement-owned storage.
- Reuse the cached tokens in `mylite_step()` for each repeated prepared
  execution.
- Keep all ownerless policy predicates, statement locks, dictionary handling,
  page-version visibility, transaction state updates, and checkpoint scheduling
  unchanged.

## Compatibility Impact

No SQL behavior, public API behavior, native storage behavior, or directory
layout changes. Unsupported-surface checks and ownerless coordination decisions
read the same token values as before.

## Performance Impact

This removes one token scan per repeated ownerless prepared execution. It is a
CPU-side hot-path cleanup; it will not erase the larger ownerless autocommit
write gap caused by the conservative flush bridge.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe`.
- Run focused ownerless prepared-read/write selectors.
- Run a reduced embedded performance probe to keep the prepared hot path
  covered.
- Run `format-check` and diff checks.

## Verification Results

Local verification on 2026-06-08 used the existing `embedded-dev` build tree.

- `cmake --build --preset embedded-dev --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- Focused ownerless prepared selectors passed:
  `prepared-committed-read`, `view-prepared-check-option`, and
  `timer-checkpoint-scheduling`.
- A reduced full-durability performance probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=1000`, and
  `MYLITE_PERF_INSERT_ITERATIONS=100`, reporting ownerless prepared
  `SELECT 1` `1442.87 ops/s`, ownerless transactional inserts
  `1059.69 ops/s`, and ownerless autocommit inserts `69.22 ops/s` on the
  loaded local host.
- Reduced durability samples before this slice showed ownerless autocommit
  insert throughput around `105.05 ops/s` at `FULL`, `106.58 ops/s` at
  `NORMAL`, and `124.81 ops/s` at `OFF`, so the remaining autocommit gap is
  not primarily MariaDB redo flush policy.
- `ctest --preset embedded-dev -R '^libmylite\.embedded-prepared-statement$'
  --output-on-failure` passed.
- `ctest --preset embedded-dev -L compat.ownerless-transaction
  --output-on-failure` passed.
- `cmake --build --preset dev --target format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Ownerless prepared statements tokenize their SQL once at prepare time.
- Existing prepared ownerless visibility and write tests pass.
- The embedded performance probe still reports ownerless prepared metrics.
- No compatibility matrix claim changes are needed because behavior is
  unchanged.
