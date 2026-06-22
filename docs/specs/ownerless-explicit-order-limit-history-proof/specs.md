# Ownerless Explicit ORDER/LIMIT DML History Proof

## Problem

The explicit ownerless transaction history proof accepts constrained
single-table `UPDATE ... WHERE ...` and `DELETE FROM ... WHERE ...` statements,
but still rejects `ORDER BY` and `LIMIT` variants. Those clauses are common
single-table row-selection controls and do not add hidden target writes when
the existing subquery, join, trigger, foreign-key, partition, and missing
`WHERE` guards remain in place.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc` plans single-table updates with `order` and
  `limit` before executing the normal handler update path. The source treats
  `ORDER BY`/`LIMIT` as reasons to avoid direct handler update optimization,
  not as separate DDL, trigger, or multi-table write semantics.
- `mariadb/sql/sql_delete.cc` plans single-table deletes with `ORDER BY` and
  `LIMIT`, removes `ORDER BY` when no limit is present, and still executes the
  normal single-table delete path with trigger handling.
- `packages/libmylite/src/database.cc`
  `ownerless_update_statement_allows_visible_fast_path()` already requires a
  direct target table, `SET`, `WHERE`, no joins, no subqueries, no partition
  shape, no locking-read token, no referential constraints, and no update
  triggers.
- `packages/libmylite/src/database.cc`
  `ownerless_delete_statement_allows_visible_fast_path()` already requires
  `DELETE FROM <target> WHERE ...`, no joins, no subqueries, no aliases, no
  partition shape, no `RETURNING`, no referential constraints, and no delete
  triggers.

## Design

Allow `ORDER` and `LIMIT` tokens in the existing constrained update and delete
statement classifiers. Do not otherwise widen the accepted grammar.

The accepted shapes are still limited to:

- explicit ownerless transactions only;
- direct single-table `UPDATE schema.table SET ... WHERE ... [ORDER BY ...]
  [LIMIT ...]` or `UPDATE table SET ... WHERE ... [ORDER BY ...] [LIMIT ...]`;
- direct single-table `DELETE FROM schema.table WHERE ... [ORDER BY ...]
  [LIMIT ...]` or `DELETE FROM table WHERE ... [ORDER BY ...] [LIMIT ...]`;
- no joined/multi-table shape, subquery/CTE token, partition syntax,
  `RETURNING`, locking-read token, temporary table, trigger-bearing target, or
  referential-constraint target/parent table.

Accepted ordered/limited statements receive the same statement-local
visible-fast marker as the simpler update/delete proof. Existing transaction
proof code then allows COMMIT fast visibility and rollback-segment/undo history
proof only if every write in the explicit transaction stayed proven.

## Scope And Non-Goals

In scope:

- Prepared single-table `UPDATE ... WHERE ... ORDER BY ... LIMIT ...` inside an
  explicit ownerless transaction.
- Prepared single-table `DELETE FROM ... WHERE ... ORDER BY ... LIMIT ...`
  inside an explicit ownerless transaction.
- Existing rollback-segment/undo history proof and page-version publication
  checks.

Out of scope:

- Joined/multi-table updates or deletes, table-alias deletes, delete-all or
  update-all statements without `WHERE`, subqueries/CTEs, `RETURNING`,
  partition forms, trigger-bearing targets, foreign-key target or parent DML,
  DDL, locking reads, savepoints, and broader native redo/checkpoint
  reconciliation.

## Compatibility Impact

SQL results and MariaDB transaction semantics are unchanged. MyLite only widens
the ownerless fast visibility proof for a bounded subset of ordinary
single-table ordered/limited DML. Rejected shapes continue to use the existing
conservative native flush path.

## Directory And Native Storage Impact

No new files, directory layout, public API, or durable WAL record formats are
introduced. Proven ordered/limited DML transactions publish through existing
ownerless page-version WAL and history-proof records inside the MyLite database
directory.

## Binary Size, License, And Dependency Impact

The slice changes first-party policy code, tests, and docs only. It adds no
dependency and has negligible binary-size impact.

## Test And Verification Plan

- Add a focused ownerless SQL selector that:
  - runs prepared ordered/limited update and delete statements in one explicit
    ownerless transaction;
  - verifies the row-order/limit effects through row counts and aggregates;
  - verifies fast COMMIT visibility, zero conservative flush, zero unproven
    statement counts, zero write-history flush pages, and positive
    rollback-segment plus undo history-proof publication; and
  - verifies ownerless reopen and forced `.shm` native reopen.
- Run the focused selector and adjacent explicit update/delete/mixed/history
  selectors.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Ordered/limited single-table update and delete statements use the
  visible-fast COMMIT and history-proof path.
- Existing subquery, trigger, FK, partition, and broad-shape exclusions remain
  conservative.
- Existing explicit update/delete/mixed proof selectors continue to pass.
- Docs and the compatibility matrix distinguish ordered/limited DML from the
  remaining broader DML and redo/checkpoint gaps.

## Verification Results

The slice is verified with:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test explicit-transaction-order-limit-history-proof`
- adjacent ownerless history selectors:
  `explicit-transaction-update-history-proof`,
  `explicit-transaction-delete-history-proof`,
  `explicit-transaction-mixed-dml-history-proof`,
  `explicit-transaction-undo-wal-elision`, and
  `single-owner-history-wal-proof`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-explicit-transaction-order-limit-history-proof$' --output-on-failure`
- `ctest --preset php-embedded-prod -R 'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace' --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Risks

- The classifier remains syntactic and conservative. It should reject
  ambiguous valid SQL rather than accept an unproven shape.
- This is not a broad update/delete proof; joined shapes, subqueries, FK
  graphs, triggers, and DDL remain separate work.
