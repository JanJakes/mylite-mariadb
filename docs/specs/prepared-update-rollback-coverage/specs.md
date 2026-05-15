# Prepared UPDATE Rollback Coverage

## Goal

Add prepared row-DML rollback coverage for a failed routed `UPDATE` that
publishes MyLite row and index changes before MariaDB reports a duplicate-key
error.

The transaction-handler hook slice already moved routed row DML onto MariaDB's
statement commit and rollback path. Existing smoke coverage includes direct
unique-key update rollback and prepared failed insert rollback. This slice
closes the prepared `UPDATE` gap without changing production code.

## Non-Goals

- Do not change public `libmylite` APIs.
- Do not add full transaction, savepoint, or multi-statement rollback support.
- Do not claim crash-safe logical undo for interrupted failed statements.
- Do not broaden coverage to triggers or foreign-key cascades.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/transaction.cc:trans_rollback_stmt()` calls
  `ha_rollback_trans(thd, FALSE)` for failed statement cleanup.
- `mariadb/sql/handler.cc:ha_rollback_trans()` dispatches rollback to
  registered handlerton participants.
- `mariadb/storage/mylite/ha_mylite.cc` registers routed write statements with
  MariaDB's statement transaction list and owns the active MyLite storage
  checkpoint for row DML.
- `packages/mylite-storage` statement checkpoints restore the header and
  catalog root snapshot, hiding row, row-state, index-entry, and autoincrement
  pages appended after the statement began.

## Compatibility Impact

Prepared row DML should follow the same failed-statement visibility rule as
direct row DML: if a prepared update fails on a later duplicate-key conflict,
no earlier row or index updates from that statement remain visible.

This remains partial compatibility. MyLite still rejects explicit transaction
control and does not claim savepoints, isolation levels, or crash-safe logical
undo for failed statements interrupted mid-rollback.

## Design

Extend the storage-engine smoke test's existing `rollback_posts` case:

- keep the direct failed unique-key update assertion;
- add a parameterized prepared `UPDATE ... WHERE id IN (2, 3) ORDER BY id`
  that tries to assign the same unique title to two rows;
- require `mylite_step()` to fail with a duplicate-key diagnostic;
- verify the duplicate title is absent through the SQL layer;
- verify both original rows still have their pre-statement titles;
- verify close/reopen keeps the same visibility.

The test uses an existing routed `ENGINE=InnoDB` table, so the requested engine
still resolves to the MyLite handler.

## Single-File And Embedded-Lifecycle Impact

No file-format or companion-file change. The test asserts that failed prepared
row DML leaves only the primary `.mylite` file and documented transient
runtime state.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

The coverage exercises the same routed MyLite handler path used by
`ENGINE=InnoDB`, omitted/default, `ENGINE=MYLITE`, `ENGINE=MyISAM`, and
`ENGINE=Aria` tables. It does not add engine-specific transaction claims.

## Build, Size, And Dependencies

No dependency or binary-size impact is expected.

## Test And Verification Plan

1. Add prepared duplicate-key update rollback coverage to
   `mylite_embedded_storage_engine_test`.
2. Verify original row values and missing duplicate title before and after
   close/reopen.
3. Run the focused storage-engine test, statement-rollback harness group,
   format checks, tidy, shell checks, and relevant preset tests.

## Acceptance Criteria

- A failed prepared unique-key update reports an execution error.
- The failed prepared update leaves no visible duplicate title.
- The original row values remain visible before and after close/reopen.
- Compatibility and roadmap docs mention prepared row-DML rollback coverage.

## Risks And Open Questions

- This proves a representative prepared `UPDATE` rollback path, not every
  generated-column, CHECK, prefix-index, or collation edge case.
