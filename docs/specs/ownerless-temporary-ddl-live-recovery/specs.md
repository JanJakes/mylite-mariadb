# Ownerless Temporary DDL Live Recovery

## Problem Statement

Ownerless connections track connection-local temporary table names so a
temporary table can shadow a permanent table without causing stale ownerless
refresh behavior. The dictionary-generation protocol is still entered for
temporary-table DDL and temporary-table rename statements because their SQL
tokens begin with `CREATE`, `DROP`, `RENAME`, or `ALTER`. If a writer dies after
MariaDB completes a temporary-table DROP or rename but before MyLite publishes
dictionary finish, the statement has no recoverable ownerless marker even
though the operation did not mutate shared durable dictionary state.

That leaves documented temporary DDL crash gaps for `DROP TEMPORARY TABLE` and
temporary-table rename variants while another ownerless peer remains live.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:13442-13445` parses `DROP [TEMPORARY] TABLE` and
  sets `SQLCOM_DROP_TABLE` with the temporary-table option.
- `mariadb/sql/sql_parse.cc:1524-1534` treats `DROP TEMPORARY TABLE` as a
  temporary-only modification for read-only and transaction checks.
- `mariadb/sql/sql_parse.cc:6346-6418` resolves `RENAME TABLE` temporary
  source tables and states that the destination is temporary when the source is
  temporary.
- `mariadb/sql/sql_table.cc:1490-1508` drops temporary tables before durable
  table deletion and handles temporary-drop warnings separately.
- `mariadb/sql/sql_table.cc:10085-10133` handles simple temporary-table
  `ALTER TABLE ... RENAME` without touching durable `.FRM` metadata.
- `packages/libmylite/src/database.cc` already tracks successful
  `CREATE TEMPORARY TABLE`, `DROP TEMPORARY TABLE`, `RENAME TABLE`, and
  `ALTER TABLE ... RENAME` temporary-table state per connection.
- The ownerless recovery classifier currently has no metadata-only recovery
  kind for temporary-table DDL, and temporary-table rename can otherwise fall
  through to the native table-rename recovery kind.

## Scope And Non-Goals

In scope:

- Add a metadata-only ownerless dictionary recovery kind for simple
  temporary-table DDL/rename prefinish boundaries.
- Recognize bounded forms:
  `CREATE TEMPORARY TABLE ...`, `DROP TEMPORARY TABLE ...`,
  single-pair `RENAME TABLE <tracked-temp> TO <name>`, and simple
  `ALTER TABLE <tracked-temp> RENAME [TO|AS|=] <name>`.
- Keep the native file-operation checkpoint-needed marker clear for those
  temporary-only recovery boundaries.
- Add hook crash selectors for `DROP TEMPORARY TABLE`, `RENAME TABLE`, and
  `ALTER TABLE ... RENAME` over connection-local temporary tables while another
  ownerless peer remains live.
- Verify the permanent table with the same name remains durable and visible
  after live recovery, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Multi-pair temporary rename chains.
- Mixed permanent and temporary rename lists.
- Temporary table DDL that also creates or drops durable permanent objects.
- Reworking ownerless statement-lock policy to avoid dictionary generations for
  temporary-only DDL entirely.
- Internal MariaDB crash points inside temporary-table engine operations.

## Design

Add `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TEMPORARY_TABLE` as a valid
metadata-only recovery kind. The kind is marked recoverable before dictionary
finish and is accepted by live-owner cleanup while the native file-operation
marker remains clear.

The classifier stays conservative:

- `CREATE TEMPORARY TABLE` and `DROP TEMPORARY TABLE` are accepted only as
  temporary-table DDL token forms, with the existing per-connection tracker
  still responsible for successful-state updates.
- `RENAME TABLE` is accepted only for one pair whose source name is already in
  the current handle's tracked temporary-table list.
- `ALTER TABLE ... RENAME` is accepted only when the target statement matches
  the existing simple rename parser and the source name is tracked as a
  temporary table.
- All mixed rename chains and untracked names remain on the existing
  conservative path.

This slice does not remove the shared dictionary generation around temporary
DDL. It only proves that the post-MariaDB-success prefinish window can be
finished by a live peer because there is no shared durable dictionary change to
repair.

## Compatibility Impact

No SQL syntax or visible MariaDB behavior changes. Temporary tables remain
connection-local and continue to shadow permanent tables in the owning
connection only. The change affects only ownerless recovery after a process
dies at a MyLite dictionary-publication boundary.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. InnoDB temporary-table
files remain under the process runtime temporary tablespace and are cleaned up
by the owning process lifetime; durable permanent tables remain in the MyLite
database directory.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact. The slice
adds one recovery-kind constant, focused classifier logic, hook tests, CTest
registration, and documentation.

## Test And Verification Plan

- Add primitive dictionary-state coverage for the new recovery kind.
- Add ownerless hook selectors for temporary DROP, `RENAME TABLE`, and
  `ALTER TABLE ... RENAME` crash recovery.
- Build `mylite_ownerless_primitives_test` and
  `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the new selectors directly and through CTest.
- Run adjacent temporary-table and temporary-rename tracking selectors.
- Run production build guards, format-check, `git diff --check`, and relevant
  ownerless stress coverage.

## Acceptance Criteria

- A killed temporary-table DROP writer can be recovered while another ownerless
  peer remains live, with the native file-operation marker clear.
- Killed temporary-table `RENAME TABLE` and `ALTER TABLE ... RENAME` writers can
  be recovered while another ownerless peer remains live, with the native
  file-operation marker clear.
- The permanent table shadowed by the temporary table remains durable and
  visible after the temporary table disappears.
- Ownerless reopen, native reopen, and forced `.shm` rebuild observe the
  permanent-table state.
- Mixed rename chains and untracked rename targets are not newly claimed.

## Risks And Follow-Up

- The recovery classifier is token-level and intentionally bounded.
- Skipping shared dictionary generations for temporary-only DDL may be a future
  cleanup, but it should be designed with statement-lock and pressure-policy
  implications instead of folded into this recovery slice.
