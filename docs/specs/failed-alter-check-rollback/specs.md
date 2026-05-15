# Failed ALTER CHECK Rollback

## Problem

MyLite already covers named CHECK constraint add/drop through supported copy
`ALTER TABLE` paths. The remaining CHECK-specific ALTER rollback gap is the
failure case where an `ALTER TABLE ... ADD CONSTRAINT ... CHECK (...)` rebuild
encounters existing rows that do not satisfy the new constraint. In that case
MariaDB should reject the ALTER, and MyLite must keep the pre-statement table
definition and rows visible.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` builds the altered
  table definition by preserving existing CHECK constraints, dropping requested
  constraints, and appending requested additions.
- `mariadb/sql/sql_table.cc:copy_data_between_tables()` copies rows into the
  altered table during copy ALTER and calls `TABLE::verify_constraints()`
  before each target handler write. A failed CHECK expression sets the thread
  error and aborts the copy.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::external_lock()` starts a
  MyLite statement checkpoint for statement-scoped handler writes and registers
  the MyLite handlerton with MariaDB transaction hooks.
- `packages/libmylite/src/database.cc:is_storage_outer_checkpoint_sql()` wraps
  top-level `CREATE`, `ALTER`, `DROP`, `RENAME`, and `TRUNCATE` statements in
  a file-backed statement checkpoint when the MyLite storage engine is enabled.
- `packages/libmylite/src/database.cc:mylite_exec()` rolls back that checkpoint
  after a MariaDB execution error before returning diagnostics to the caller.

## Design

Do not add MyLite-native CHECK parsing or special-case constraint evaluation.
Use MariaDB's copy ALTER failure path, and prove that MyLite's existing
statement checkpoint restores pre-ALTER catalog and row visibility.

Extend storage-engine smoke coverage on the existing `checked_posts` routed
table:

- create rows that satisfy the current `rating <= 9` ALTER-added CHECK,
- attempt to add a stricter named CHECK that existing rows violate,
- verify the statement fails with a constraint diagnostic,
- verify the original table definition and rows remain usable,
- insert another row that would have violated the failed CHECK but still
  satisfies the previously published CHECK, and
- close/reopen to prove the failed CHECK was not persisted in catalog-backed
  table metadata.

## Supported Scope

- Failed named table-level CHECK additions on supported MyLite-routed tables
  when existing rows violate the new constraint.
- Restoration of pre-statement MyLite catalog and row visibility through the
  current statement-checkpoint path.
- Close/reopen proof that the failed CHECK definition was not published.

## Non-Goals

- Full DDL undo for every failed copy ALTER shape.
- Transaction rollback, savepoints, or multi-statement rollback.
- Broad CHECK expression matrices.
- Prepared-statement-specific diagnostics.
- Column-level generated CHECK names, duplicate CHECK diagnostics, or
  `ADD CONSTRAINT IF NOT EXISTS` semantics.

## Compatibility Impact

CHECK constraints remain partial support, but failed `ALTER TABLE ... ADD
CONSTRAINT ... CHECK` over incompatible existing rows moves from planned to
covered for supported routed table shapes.

## DDL Metadata Routing Impact

A failed ADD CHECK must not publish the altered MariaDB table-definition image
to the MyLite catalog. The old definition remains discoverable in the same
opened runtime and after close/reopen.

## Single-File And Embedded-Lifecycle Impact

No new file or companion type is introduced. Any pages appended while MariaDB
attempts the failed copy ALTER remain unreachable until compaction exists, but
the visible header/catalog state is restored by the existing statement
checkpoint.

## Public API And File-Format Impact

No public `libmylite` API or storage format change is needed.

## Storage-Engine Routing Impact

The behavior applies through the shared MyLite handler and catalog path for
supported routed engine requests. The smoke test uses `ENGINE=InnoDB`, the
main compatibility target for application schemas.

## Binary-Size And Dependency Impact

No dependency is added. Binary-size impact is limited to test and documentation
text unless the existing checkpoint path needs a bug fix.

## Test And Verification Plan

- Extend storage-engine smoke coverage for failed ADD CHECK rollback on a
  routed `ENGINE=InnoDB` table.
- Verify same-runtime inserts prove the failed CHECK is not active.
- Verify close/reopen inserts prove the failed CHECK was not persisted.
- Update CHECK, rollback, storage, compatibility, roadmap, and harness docs.
- Run format, targeted storage-smoke tests, compatibility harness reports,
  tidy, full preset tests, shell checks, and `git diff --check`.

## Acceptance Criteria

- Failed ADD CHECK over incompatible existing rows returns an error.
- Existing rows and catalog metadata remain visible after the failed ALTER.
- A row that satisfies the old CHECK but violates the failed new CHECK can be
  inserted before and after close/reopen.
- Compatibility docs no longer list failed ADD CHECK rollback as planned.

## Risks And Unresolved Questions

- This slice proves visible statement rollback, not physical reclamation of
  pages appended during the failed copy ALTER.
- Other failed copy ALTER shapes still need targeted coverage before MyLite can
  claim broad DDL rollback compatibility.
