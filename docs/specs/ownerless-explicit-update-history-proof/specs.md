# Ownerless Explicit Update History Proof

## Problem

The explicit ownerless transaction history proof was limited to eligible
`INSERT ... VALUES` statements. Simple `UPDATE` statements inside the same
transaction model still disqualified the later `COMMIT`, forcing the
conservative native dirty-page and write-history flush path even when the
transaction had no savepoints, locking reads, DDL, foreign keys, or dictionary
refresh boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` is the first-party SQL policy gate
  that marks a statement with `OwnerlessStatementVisibleFastPathScope`.
- `packages/libmylite/src/database.cc`
  `update_ownerless_explicit_transaction_visible_fast_proof_before_sql()` uses
  that statement-local marker to carry a transaction-scoped proof to the later
  `COMMIT`.
- `mariadb/storage/innobase/trx/trx0trx.cc` accepts ownerless fast visibility
  only after transaction pages are published, no publish failure is recorded,
  deferred pages are proved, and at least one page was published.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` still requires the active
  rollback-segment and undo history-proof pages before it skips the exact native
  history flush.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  tracks page writes for non-autocommit SQL transactions even when the
  statement visible-fast marker is set; the autocommit skip is separate.

## Design

Add a conservative `UPDATE` statement classifier to the existing ownerless
fast-path policy.

The classifier only accepts direct single-table `UPDATE schema.table SET ...
WHERE ...` or `UPDATE table SET ... WHERE ...` shapes. It rejects:

- multi-table and joined updates,
- modifier/table-partition shapes,
- `UPDATE` statements without `WHERE`,
- subquery and `RETURNING` shapes,
- tracked temporary tables, and
- tables that participate in referential constraints either as child or
  referenced parent tables, and
- target tables that have `UPDATE` triggers.

Accepted `UPDATE` statements receive the same statement-local visible-fast
marker that eligible inserts already use. Existing transaction proof code then
allows the following `COMMIT` to use the fast visibility and history proof only
if every write in the explicit transaction stayed proven and no savepoint,
locking read, DDL, or conservative dictionary refresh disqualified it.
Successful referential-constraint and trigger metadata lookups are cached per
handle and observed dictionary generation, matching the existing insert
metadata cache pattern so repeated updates of the same table do not re-query
`information_schema`.

## Scope And Non-Goals

In scope:

- Direct and prepared simple single-table `UPDATE ... SET ... WHERE ...`
  statements inside explicit ownerless transactions, including the follow-up
  ordered/limited shape documented in
  `../ownerless-explicit-order-limit-history-proof/specs.md`.
- Trigger-table rejection for constrained explicit update proofs.
- Existing rollback-segment/undo history proof and page-version publication
  checks.
- Focused SQL coverage for positive simple updates, a subquery-update negative
  proof, ordered/limited update proof, and trigger-table conservative fallback.

Out of scope:

- `DELETE`, `REPLACE`, `INSERT ... SELECT`, joined or subquery update shapes,
  DDL, locking reads, savepoints, trigger-bearing updates, and foreign-key
  target or parent updates.
- Broader redo/checkpoint reconciliation and DDL/file-lifecycle recovery.
- External MariaDB/RQG randomized DML stress.

## Compatibility Impact

SQL results and MariaDB transaction semantics are unchanged. The change only
widens the MyLite ownerless fast visibility proof for a constrained update
shape. Rejected update shapes remain correct through the existing conservative
native flush path.

## Directory And Native Storage Impact

No new files, directory layout, public API, or durable WAL record formats are
introduced. Proven update transactions publish through existing ownerless
page-version WAL and history-proof records inside the MyLite database
directory.

## Binary Size, License, And Dependency Impact

The slice changes first-party policy code, tests, and docs only. It adds no
dependency and has negligible binary-size impact.

## Test And Verification Plan

- Add a focused ownerless SQL selector that:
  - runs prepared simple updates in one explicit ownerless transaction;
  - verifies fast COMMIT visibility, zero conservative flush, zero unproven
    statement counts, zero write-history flush pages, and positive
    rollback-segment plus undo history-proof publication;
  - verifies same-handle visibility, ownerless reopen, and forced `.shm`
    native reopen;
  - verifies an explicit transaction containing an update with a subquery stays
    on the conservative unproven path; and
  - verifies an explicit transaction targeting an update-trigger table stays on
    the conservative unproven path while preserving trigger side effects.
- Run the focused selector and adjacent history/native-support selectors.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Constrained explicit-transaction updates use the visible-fast COMMIT and
  history-proof path.
- Subquery updates and update-trigger target tables do not enter the proof.
- Existing insert proof selectors continue to pass.
- Docs and the compatibility matrix distinguish the new update proof from the
  remaining DML, redo/checkpoint, and external-stress gaps.

## Verification Results

The slice is verified with:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test explicit-transaction-update-history-proof`
- adjacent ownerless history/native-support selectors:
  `explicit-transaction-undo-wal-elision`,
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`, and
  `single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-explicit-transaction-update-history-proof$' --output-on-failure`
- `ctest --preset php-embedded-prod -R 'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace' --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Risks

- The SQL classifier is intentionally syntactic and conservative. It should
  reject ambiguous valid SQL rather than accept an unproven shape.
- The referential-constraint metadata query is fail-closed. A metadata lookup
  failure keeps the update on the conservative path.
- Trigger metadata lookup is also fail-closed, keeping trigger-bearing or
  unproven trigger-state tables on the conservative path.
- This is not a broad DML proof; `DELETE`, `REPLACE`, `INSERT ... SELECT`,
  joined/subquery update and foreign-key update matrices remain separate work.
