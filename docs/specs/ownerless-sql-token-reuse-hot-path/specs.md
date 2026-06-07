# Ownerless SQL Token Reuse Hot Path

## Problem

The embedded performance probe now makes ownerless startup and core-engine
costs visible separately from WordPress build/setup time. A current local run
showed ordinary and ownerless warm open/close within the same range, but
ownerless steady-state SQL still paid extra per-statement overhead:

- ordinary warm open/close `426.326ms`,
- ownerless warm open/close `429.676ms`,
- ordinary direct `SELECT 1` `4300.76 ops/s`,
- ownerless direct `SELECT 1` `3407.54 ops/s`,
- ordinary prepared `SELECT 1` `2229.04 ops/s`,
- ownerless prepared `SELECT 1` `1827.55 ops/s`.

Ownerless writes necessarily pay page-write and redo coordination costs. Reads
still need dictionary refresh, snapshot visibility, and policy checks, but the
current direct and prepared ownerless paths repeatedly tokenize the same SQL
text while making those decisions. That is avoidable CPU overhead on the
ownerless hot path.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:exec_impl()` already collects
  `SqlPolicyTokens` for the ownerless direct path before pressure checks.
- The same ownerless direct statement then re-tokenizes through
  `acquire_ownerless_statement_locks()`,
  `statement_allows_ownerless_page_version_reads()`,
  `ownerless_begin_dictionary_ddl()`,
  `update_current_schema_after_successful_sql()`, and
  `update_ownerless_transaction_state_after_successful_sql()`.
- `packages/libmylite/src/database.cc:mylite_step()` has the same repeated
  tokenization for ownerless prepared statements after retaining the prepared
  SQL text.
- The MariaDB/InnoDB correctness boundaries stay in the existing ownerless
  hooks under `mariadb/storage/innobase`; this slice does not change lock,
  redo, page-version, dictionary, or recovery behavior.

## Design

Reuse the already-collected `SqlPolicyTokens` through the ownerless direct and
prepared statement path:

- have `acquire_ownerless_statement_locks()` accept tokens instead of SQL text,
- have `statement_allows_ownerless_page_version_reads()` accept tokens,
- have `ownerless_begin_dictionary_ddl()` accept tokens,
- have `update_ownerless_transaction_state_after_successful_sql()` accept
  tokens,
- add a token-based overload for `update_current_schema_after_successful_sql()`
  and use it in the ownerless direct path.

The ordinary direct SQL path keeps its existing behavior. The ownerless path
keeps the same classification predicates and executes them in the same order;
only repeated token collection is removed.

## Compatibility Impact

No SQL, public API, PHP API, or storage-engine behavior changes. Unsupported
policy checks, dictionary DDL coordination, page-version visibility, transaction
state updates, and statement locks use the same token predicates as before.

## Directory And Lifecycle Impact

No directory-layout or lifecycle changes.

## Native Storage Impact

No native storage format changes. InnoDB hooks, redo visibility, page-version
WAL publication, and recovery paths are unchanged.

## Build And Performance Impact

This is a CPU-side ownerless hot-path cleanup. It should reduce the overhead of
short ownerless direct and prepared SQL statements without affecting ordinary
WordPress mysqli opens, which do not request `MYLITE_OPEN_OWNERLESS_RW`.

The expected improvement is modest because MariaDB execution and ownerless
coordination still dominate many statements, especially writes.

## Test Plan

- Build `mylite_embedded_performance_probe` in `embedded-dev`.
- Run the embedded performance probe before and after the change with the same
  iteration counts.
- Run focused ownerless cross-process SQL coverage that exercises reads, writes,
  transactions, and DDL state updates.
- Run focused PHP CTest coverage to keep the WordPress mysqli ordinary path
  guarded.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-07 used the pinned CI WordPress ref
`6ddfc9d9b532c6e95c1266165149815895e2eb56` and the default host-temp
WordPress database placement.

- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe` passed.
- Before the code change, the embedded probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=20`,
  `MYLITE_PERF_SELECT_ITERATIONS=10000`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported ordinary warm open/close
  `426.326ms`, ownerless warm open/close `429.676ms`, ordinary direct
  `SELECT 1` `4300.76 ops/s`, ownerless direct `SELECT 1` `3407.54 ops/s`,
  ordinary prepared `SELECT 1` `2229.04 ops/s`, ownerless prepared
  `SELECT 1` `1827.55 ops/s`, ordinary inserts `2098.52 ops/s`, and
  ownerless inserts `1228.98 ops/s`.
- A small `strace -c -f` probe showed the reduced run dominated by MariaDB
  thread/futex behavior and open/close work, not an unexpected burst of
  steady-read filesystem operations.
- After the code change, the same embedded probe reported ordinary warm
  open/close `512.429ms`, ownerless warm open/close `437.256ms`, ordinary
  direct `SELECT 1` `4074.05 ops/s`, ownerless direct `SELECT 1`
  `3307.30 ops/s`, ordinary prepared `SELECT 1` `2009.54 ops/s`, ownerless
  prepared `SELECT 1` `1882.18 ops/s`, ordinary inserts `1800.65 ops/s`, and
  ownerless inserts `1310.51 ops/s`. Absolute rates moved with host load, but
  ownerless/ordinary ratios improved from `0.79` to `0.81` for direct reads,
  `0.82` to `0.94` for prepared reads, and `0.59` to `0.73` for the insert
  transaction loop.
- `cmake --build --preset embedded-dev --target
  mylite_ownerless_cross_process_sql_test` passed.
- Focused ownerless SQL cases passed:
  `test_transaction_first_read_sees_committed_external_update`,
  `test_prepared_transaction_first_read_sees_committed_external_update`,
  `test_consistent_snapshot_transaction_hides_later_external_update`, and
  `test_ownerless_alter_waits_for_active_transaction`.
- `ctest --preset embedded-dev -L compat.ownerless-transaction
  --output-on-failure` passed.
- `ctest --preset embedded-dev -L compat.ownerless-cross-process-sql
  --parallel 2 --output-on-failure` passed all 16 weighted shards in
  `293.11s` real time.
- `cmake --build --preset php-embedded-dev --target mylite_php_extension
  mylite_mysqli_php_extension` passed.
- `ctest --preset php-embedded-dev -L php --output-on-failure` passed.
- `MYLITE_WORDPRESS_PHASE=build-php MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  tools/wordpress-phpunit-mysqli-mylite` passed with
  `mylite_build_seconds=19` and `wordpress_total_seconds=22`.
- `MYLITE_WORDPRESS_PHASE=prepare-db MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  tools/wordpress-phpunit-mysqli-mylite` passed with
  `wordpress_prepare_db_seconds=2`.
- The CI-sized WordPress `perf-probe` passed and reported stock PHP process
  startup `56.197ms`, PHP wrapper startup `71.241ms`, process plus MyLite
  connect/close `567.413ms`, derived process/connect delta `496.172ms`,
  in-process mysqli connect/close `405.098ms`, `SELECT 1` `258.41 ops/s`,
  transactional inserts `419.33 ops/s`, primary-key point selects
  `235.38 ops/s`, and `wordpress_total_seconds=16`.
- `cmake --build --preset format-check` passed.

## Acceptance Criteria

- Ownerless direct/prepared statement paths reuse one token set for their local
  policy and state decisions.
- Existing ownerless correctness tests pass.
- The embedded performance probe still succeeds and reports ownerless startup
  and engine metrics.
- No compatibility matrix claim changes are needed because semantics are
  unchanged.
