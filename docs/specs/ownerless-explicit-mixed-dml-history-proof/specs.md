# Ownerless Explicit Mixed DML History Proof

## Problem

Explicit ownerless transactions now have separate history proofs for eligible
`INSERT ... VALUES`, constrained `UPDATE ... SET ... WHERE ...`, constrained
`DELETE FROM ... WHERE ...`, and constrained `REPLACE ... VALUES` statements.
The shared transaction proof, however, still lacks focused evidence that those
accepted statement classes can be combined in one explicit transaction without
falling back to the conservative unproven-statement dirty-page and
write-history flush path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `ownerless_history_wal_proof_allows_fast_path()` uses the MyLite
  visible-fast statement marker as the InnoDB history-WAL proof boundary after
  read-only and dictionary-operation checks.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` publishes the active rollback-segment
  and undo-header pages and skips the native ownerless write-history page flush
  only when both history-proof pages are published without page-publish failure.
- `mariadb/sql/sql_insert.cc` routes `INSERT` and `REPLACE` row writes through
  `mysql_insert()` and `Write_record::write_record()`, with `REPLACE` carrying
  the documented delete-plus-insert and auto-increment caveats already handled
  by the per-statement replace classifier.
- `mariadb/sql/sql_update.cc` executes accepted single-table updates through
  the normal handler update path and trigger checks already considered by the
  per-statement update classifier.
- `mariadb/sql/sql_delete.cc` executes accepted single-table deletes through
  the normal handler delete path and trigger checks already considered by the
  per-statement delete classifier.
- `packages/libmylite/src/database.cc`
  `update_ownerless_explicit_transaction_visible_fast_proof_before_sql()` is
  statement-class agnostic: every write inside an explicit transaction must be
  accepted by `ownerless_statement_fast_path_policy()`, and any unproven write,
  savepoint operation, or locking read disqualifies the later COMMIT marker.

## Design

Do not widen SQL grammar in this slice. Instead, add focused SQL coverage that
combines all currently proven statement classes inside one explicit ownerless
transaction:

- prepared `INSERT ... VALUES`,
- prepared constrained single-table `UPDATE ... SET ... WHERE ...`,
- prepared constrained single-table `DELETE FROM ... WHERE ...`, and
- prepared constrained single-table `REPLACE ... VALUES`.

The positive transaction uses one InnoDB table with no foreign keys, triggers,
auto-increment columns, temporary-table state, DDL, savepoints, or locking
reads. The transaction must preserve the existing visible-fast COMMIT proof,
publish rollback-segment and undo history-proof pages, and report zero
conservative unproven-statement or ownerless write-history flush work.

The same selector also proves that one unproven statement still poisons a mixed
explicit transaction: after one proven write, a subquery update remains on the
conservative path and the following COMMIT must report unproven-statement
fallback while preserving SQL results.

## Scope And Non-Goals

In scope:

- Mixed explicit transactions whose component writes are already eligible under
  the existing insert, update, delete, and replace classifiers.
- A negative mixed transaction with one unproven write.
- Existing rollback-segment/undo history proof and page-version publication
  checks.

Out of scope:

- `INSERT ... SELECT`, `REPLACE ... SELECT`, broader update/delete/replace
  shapes, DDL, locking reads, savepoints beyond existing negative proof,
  foreign-key target or parent DML, auto-increment replacement semantics, and
  broader native redo/checkpoint reconciliation.

## Compatibility Impact

SQL results and MariaDB transaction semantics are unchanged. The slice proves
that MyLite's ownerless transaction proof state can span multiple already
accepted DML statement classes. Rejected mixed transactions remain correct
through the existing conservative native flush path.

## Directory And Native Storage Impact

No new files, directory layout, public API, or durable WAL record formats are
introduced. Proven mixed-DML transactions publish through existing ownerless
page-version WAL and history-proof records inside the MyLite database
directory.

## Binary Size, License, And Dependency Impact

The slice changes tests and docs only unless the focused selector exposes a
missing state-machine implementation gap. It adds no dependency and has
negligible binary-size impact.

## Test And Verification Plan

- Add a focused ownerless SQL selector that:
  - runs prepared insert, update, delete, and replace statements in one
    explicit ownerless transaction;
  - verifies fast COMMIT visibility, zero conservative flush, zero unproven
    statement counts, zero write-history flush pages, and positive
    rollback-segment plus undo history-proof publication;
  - verifies same-handle visibility, ownerless reopen, and forced `.shm`
    native reopen; and
  - verifies a mixed transaction containing one unproven subquery update stays
    on the conservative unproven path while preserving SQL side effects.
- Run the focused selector and adjacent explicit insert/update/delete/replace
  history selectors.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- A mixed transaction made only of already-proven DML statements uses the
  visible-fast COMMIT and history-proof path.
- One unproven write inside a mixed transaction disqualifies the whole COMMIT.
- Existing individual insert/update/delete/replace proof selectors continue to
  pass.
- Docs and the compatibility matrix distinguish mixed proven DML from the
  remaining broader DML, redo/checkpoint, and external-stress gaps.

## Verification Results

The slice is verified with:

- `cmake --build --preset php-embedded-prod --target mylite_ownerless_cross_process_sql_test`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test explicit-transaction-mixed-dml-history-proof`
- adjacent ownerless history/native-support selectors:
  `explicit-transaction-undo-wal-elision`,
  `explicit-transaction-update-history-proof`,
  `explicit-transaction-delete-history-proof`,
  `explicit-transaction-replace-history-proof`,
  `single-owner-history-wal-proof`, and
  `single-owner-native-support-page-wal-elision`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-explicit-transaction-mixed-dml-history-proof$' --output-on-failure`
- `ctest --preset php-embedded-prod -R 'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace' --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Risks

- This proof depends on the per-statement classifiers staying conservative.
  Future classifier broadening must preserve the all-writes-proven transaction
  invariant.
- This is not a broad DML proof; select-source inserts/replacements,
  trigger-bearing targets, foreign-key graphs, auto-increment replacement, DDL,
  and locking-read transactions remain separate work.
