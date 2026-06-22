# Ownerless Explicit Replace History Proof

## Problem

Explicit ownerless transactions now carry the history proof across eligible
`INSERT ... VALUES`, constrained `UPDATE ... SET ... WHERE ...`, and
constrained `DELETE FROM ... WHERE ...` statements. `REPLACE ... VALUES`
remains unproven, so explicit transactions that use simple replacement writes
fall back to the conservative dirty-page and write-history flush path even when
the target table has no foreign keys, triggers, auto-increment columns, DDL, or
dictionary-refresh boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `REPLACE` through the same
  `insert_table`/`insert_field_spec` grammar family as `INSERT`, with optional
  `LOW_PRIORITY`/`DELAYED`, optional `INTO`, value lists, `REPLACE ... SELECT`,
  and `RETURNING` variants.
- `mariadb/sql/sql_insert.cc` documents `REPLACE` as either an insert or a
  delete plus insert; it can optimize to an update only when triggers and
  referenced-key effects do not require the delete/insert semantics.
- `mariadb/sql/sql_insert.cc` also treats auto-increment replacement specially
  and avoids replacing a row when an auto-increment column was used for the
  conflicting key path, so the first ownerless proof should reject
  auto-increment targets.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` is the first-party SQL policy gate
  that marks a statement with `OwnerlessStatementVisibleFastPathScope`.
- `packages/libmylite/src/database.cc`
  `update_ownerless_explicit_transaction_visible_fast_proof_before_sql()` carries
  accepted statement-local markers to the later explicit transaction `COMMIT`.
- `mariadb/storage/innobase/trx/trx0trx.cc` accepts ownerless fast visibility
  only after transaction pages are published and then requires rollback-segment
  plus undo history-proof pages before skipping the ownerless write-history
  flush.

## Design

Add a conservative `REPLACE` statement classifier to the ownerless fast-path
policy, but only for explicit ownerless transactions.

The classifier accepts direct or prepared single-table
`REPLACE [INTO] schema.table ... VALUES (...)` and
`REPLACE [INTO] table ... VALUES (...)` shapes. It rejects:

- `LOW_PRIORITY` and `DELAYED` modifiers,
- explicit `PARTITION` target clauses,
- `REPLACE ... SELECT`,
- `RETURNING`, `WITH`, `SELECT`, and joined shapes,
- tracked temporary tables,
- target tables that participate in referential constraints either as child or
  referenced parent tables,
- target tables with any trigger, because `REPLACE` can fire insert and delete
  trigger semantics, and
- target tables with auto-increment columns.

Accepted `REPLACE` statements receive the same statement-local visible-fast
marker as eligible explicit update/delete statements. Existing transaction
proof code then allows the following `COMMIT` to use fast visibility and the
rollback-segment/undo history WAL proof only if every write in the explicit
transaction stayed proven and no savepoint, locking read, DDL, trigger,
foreign-key, auto-increment, or conservative dictionary refresh disqualified
it.

Successful referential-constraint, trigger, and auto-increment metadata lookups
are cached per handle and observed dictionary generation. Metadata lookup
failures remain fail-closed.

## Scope And Non-Goals

In scope:

- Direct and prepared simple single-table `REPLACE ... VALUES` statements
  inside explicit ownerless transactions.
- Strict metadata rejection for referential constraints, triggers, and
  auto-increment targets.
- Existing rollback-segment/undo history proof and page-version publication
  checks.
- Focused SQL coverage for positive simple replacements plus conservative
  fallback for `REPLACE ... SELECT`, trigger targets, and auto-increment
  targets.

Out of scope:

- `INSERT ... SELECT`, `REPLACE ... SELECT`, broader update/delete shapes,
  DDL, locking reads, savepoints, trigger-bearing replacement semantics,
  auto-increment replacement semantics, and foreign-key target or parent DML.
- Broader native redo/checkpoint reconciliation and DDL/file-lifecycle recovery.
- External MariaDB/RQG randomized DML stress.

## Compatibility Impact

SQL results and MariaDB transaction semantics are unchanged. The change only
widens the MyLite ownerless fast visibility proof for a constrained replace
shape. Rejected replacement shapes remain correct through the existing
conservative native flush path.

## Directory And Native Storage Impact

No new files, directory layout, public API, or durable WAL record formats are
introduced. Proven replace transactions publish through existing ownerless
page-version WAL and history-proof records inside the MyLite database
directory.

## Binary Size, License, And Dependency Impact

The slice changes first-party policy code, tests, and docs only. It adds no
dependency and has negligible binary-size impact.

## Test And Verification Plan

- Add a focused ownerless SQL selector that:
  - runs prepared simple replacements in one explicit ownerless transaction;
  - verifies fast COMMIT visibility, zero conservative flush, zero unproven
    statement counts, zero write-history flush pages, and positive
    rollback-segment plus undo history-proof publication;
  - verifies same-handle visibility, ownerless reopen, and forced `.shm`
    native reopen;
  - verifies `REPLACE ... SELECT` stays on the conservative unproven path;
  - verifies trigger-bearing replacement targets stay on the conservative
    unproven path while preserving trigger side effects; and
  - verifies auto-increment replacement targets stay on the conservative
    unproven path.
- Run the focused selector and adjacent explicit insert/update/delete history
  selectors.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Constrained explicit-transaction replacements use the visible-fast COMMIT and
  history-proof path.
- `REPLACE ... SELECT`, trigger-bearing replacement targets, and auto-increment
  replacement targets do not enter the proof.
- Existing insert/update/delete proof selectors continue to pass.
- Docs and the compatibility matrix distinguish the new replacement proof from
  the remaining DML, redo/checkpoint, and external-stress gaps.

## Verification Results

The slice is verified with:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test explicit-transaction-replace-history-proof`
- adjacent ownerless history/native-support selectors:
  `explicit-transaction-delete-history-proof`,
  `explicit-transaction-update-history-proof`,
  `explicit-transaction-undo-wal-elision`,
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`, and
  `single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-explicit-transaction-replace-history-proof$' --output-on-failure`
- `ctest --preset php-embedded-prod -R 'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace' --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Risks

- The SQL classifier is intentionally syntactic and conservative. It should
  reject ambiguous valid SQL rather than accept an unproven shape.
- Trigger, referential-constraint, and auto-increment metadata queries are
  fail-closed. A metadata lookup failure keeps the statement on the conservative
  path.
- This is not a broad replacement proof; `REPLACE ... SELECT`, auto-increment
  replacement, triggers, foreign-key replacement matrices, and external oracle
  stress remain separate work.
