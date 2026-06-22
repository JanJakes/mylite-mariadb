# Ownerless Explicit Delete History Proof

## Problem

The explicit ownerless transaction history proof now covers eligible
`INSERT ... VALUES` and constrained single-table `UPDATE ... SET ... WHERE ...`
statements. Simple `DELETE FROM ... WHERE ...` statements still disqualify the
later `COMMIT`, forcing the conservative native dirty-page and write-history
flush path even when the transaction has no savepoints, locking reads, DDL,
foreign keys, triggers, or dictionary-refresh boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses single-table `DELETE` as
  `DELETE ... FROM table_ident ... WHERE ... ORDER ... LIMIT ... RETURNING`,
  while multi-table forms become `SQLCOM_DELETE_MULTI` through either
  `DELETE table_alias_ref_list FROM join_table_list ...` or
  `DELETE FROM table_alias_ref_list USING join_table_list ...`.
- `mariadb/sql/sql_delete.cc` can optimize no-`WHERE` delete-all statements and
  explicitly considers delete triggers, so the ownerless proof must require a
  `WHERE` clause and reject trigger-bearing target tables.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` is the first-party SQL policy gate
  that marks a statement with `OwnerlessStatementVisibleFastPathScope`.
- `packages/libmylite/src/database.cc`
  `update_ownerless_explicit_transaction_visible_fast_proof_before_sql()` carries
  the statement-local marker to the later explicit transaction `COMMIT`.
- `mariadb/storage/innobase/trx/trx0trx.cc` accepts ownerless fast visibility
  only after transaction pages are published and then requires the active
  rollback-segment and undo history-proof pages before skipping the ownerless
  write-history flush.

## Design

Add a conservative `DELETE` statement classifier to the existing ownerless
fast-path policy.

The classifier only accepts direct single-table `DELETE FROM schema.table
WHERE ...` or `DELETE FROM table WHERE ...` shapes inside explicit ownerless
transactions. It rejects:

- delete modifiers and history/period/table-partition forms,
- multi-table and joined deletes,
- table aliases,
- `DELETE` statements without `WHERE`,
- subquery and `RETURNING` shapes,
- tracked temporary tables,
- target tables that participate in referential constraints either as child or
  referenced parent tables, and
- target tables that have triggers for the relevant DML operation.

The trigger rejection is also applied to the existing constrained update proof.
Triggers can execute additional SQL not represented by the target-table
classifier, so proven explicit DML remains limited to statements whose
side-effects are visible in the statement text and existing storage hooks.

Accepted `DELETE` statements receive the same statement-local visible-fast
marker that eligible inserts and updates use. Existing transaction proof code
then allows the following `COMMIT` to use fast visibility and the rollback-
segment/undo history WAL proof only if every write in the explicit transaction
stayed proven and no savepoint, locking read, DDL, trigger, foreign-key, or
conservative dictionary refresh disqualified it.

Successful referential-constraint and trigger metadata lookups are cached per
handle and observed dictionary generation. Metadata lookup failures remain
fail-closed.

## Scope And Non-Goals

In scope:

- Direct and prepared simple single-table `DELETE FROM ... WHERE ...`
  statements inside explicit ownerless transactions, including the follow-up
  ordered/limited shape documented in
  `../ownerless-explicit-order-limit-history-proof/specs.md`.
- Trigger-table rejection for constrained explicit update/delete proofs.
- Existing rollback-segment/undo history proof and page-version publication
  checks.
- Focused SQL coverage for positive simple deletes, a subquery-delete negative
  proof, ordered/limited delete proof, and update/delete trigger-table
  conservative fallback.

Out of scope:

- `REPLACE`, `INSERT ... SELECT`, broader `UPDATE` shapes, joined/multi-table
  `DELETE`, table-alias deletes, delete-all/no-`WHERE` optimization, subquery
  deletes, DDL, locking reads, savepoints, and foreign-key target or parent
  DML.
- Broader native redo/checkpoint reconciliation and DDL/file-lifecycle recovery.
- External MariaDB/RQG randomized DML stress.

## Compatibility Impact

SQL results and MariaDB transaction semantics are unchanged. The change only
widens the MyLite ownerless fast visibility proof for a constrained delete
shape and tightens the update proof for trigger-bearing tables. Rejected shapes
remain correct through the existing conservative native flush path.

## Directory And Native Storage Impact

No new files, directory layout, public API, or durable WAL record formats are
introduced. Proven delete transactions publish through existing ownerless
page-version WAL and history-proof records inside the MyLite database
directory.

## Binary Size, License, And Dependency Impact

The slice changes first-party policy code, tests, and docs only. It adds no
dependency and has negligible binary-size impact.

## Test And Verification Plan

- Add a focused ownerless SQL selector that:
  - runs prepared simple deletes in one explicit ownerless transaction;
  - verifies fast COMMIT visibility, zero conservative flush, zero unproven
    statement counts, zero write-history flush pages, and positive
    rollback-segment plus undo history-proof publication;
  - verifies same-handle visibility, ownerless reopen, and forced `.shm`
    native reopen;
  - verifies an explicit transaction containing a delete with a subquery stays
    on the conservative unproven path; and
  - verifies trigger-bearing update and delete target tables stay on the
    conservative unproven path while preserving trigger side effects.
- Run the focused selector and adjacent history/update/native-support
  selectors.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Constrained explicit-transaction deletes use the visible-fast COMMIT and
  history-proof path.
- Subquery deletes and trigger-bearing update/delete target tables do not enter
  the proof.
- Existing insert and update proof selectors continue to pass.
- Docs and the compatibility matrix distinguish the new delete proof from the
  remaining DML, redo/checkpoint, and external-stress gaps.

## Verification Results

The slice is verified with:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test explicit-transaction-delete-history-proof`
- adjacent ownerless history/native-support selectors:
  `explicit-transaction-update-history-proof`,
  `explicit-transaction-undo-wal-elision`,
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`, and
  `single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-explicit-transaction-delete-history-proof$' --output-on-failure`
- `ctest --preset php-embedded-prod -R 'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace' --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Risks

- The SQL classifier is intentionally syntactic and conservative. It should
  reject ambiguous valid SQL rather than accept an unproven shape.
- Trigger and referential-constraint metadata queries are fail-closed. A
  metadata lookup failure keeps the statement on the conservative path.
- This is not a broad DML proof; `REPLACE`, `INSERT ... SELECT`, broad update
  and delete shapes beyond ordered/limited single-table forms, and foreign-key
  DML matrices remain separate work.
