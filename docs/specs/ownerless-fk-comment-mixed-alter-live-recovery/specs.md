# Ownerless FK Comment Mixed ALTER Live Recovery

## Problem Statement

Ownerless FK DDL live recovery covers ordinary FK add/drop lists and one mixed
FK/non-FK list that adds a stored column. The remaining mixed-list gap includes
metadata-only non-FK clauses. A process that dies after MariaDB applies a
single `ALTER TABLE` list containing `DROP FOREIGN KEY`, table `COMMENT`, and
`ADD CONSTRAINT ... FOREIGN KEY`, but before MyLite finishes ownerless
dictionary state, must be recoverable while another ownerless peer remains
live.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` accepts table `COMMENT` options in ALTER-table
  option lists and records `HA_CREATE_USED_COMMENT` in `Lex->create_info`.
- `mariadb/sql/sql_yacc.yy` records table-level FK clauses through
  `Lex->add_table_foreign_key()`.
- `mariadb/sql/sql_table.cc` executes ALTER table lists through
  `mysql_alter_table()`, including FK metadata checks and table-option
  metadata updates.
- MyLite's `packages/libmylite/src/database.cc`
  `ownerless_alter_table_mixed_foreign_key_recovery_statement()` already
  classifies bounded mixed FK drop/add lists and the existing FK plus
  `ADD COLUMN` case.

## Design

Extend the bounded mixed-FK classifier to also accept one or more table
`COMMENT = <literal>` clauses inside a list that still contains at least one
FK add and one FK drop. Classify the list as FK metadata recovery unless an
`ADD COLUMN` clause is present. This keeps the generated-column FK guard,
referenced-table checks, and native file-operation marker behavior unchanged.

Add a hook-build SQL test that:

1. Creates a child table with two existing FKs and an initial table comment.
2. Kills a writer at `dictionary-before-finish` after
   `DROP FOREIGN KEY`, `COMMENT = ...`, and
   `ADD CONSTRAINT ... FOREIGN KEY` succeed natively.
3. Keeps another ownerless peer live during recovery.
4. Verifies the new table comment, dropped/retained/added FK metadata, FK
   enforcement, native dictionary marker retention and final drain,
   ownerless/native reopen, and forced `.shm` rebuild behavior.

## Scope And Non-Goals

In scope:

- One metadata-only non-FK table option: table `COMMENT`.
- FK drop plus FK add in the same ALTER list.
- Child/referenced tables without generated columns.
- Live-peer dictionary recovery at the existing post-native
  `dictionary-before-finish` hook.

Out of scope:

- Arbitrary mixed ALTER lists.
- Generated-column child or referenced FK metadata-only live recovery.
- Storage-rebuild, column, index, CHECK, trigger, view, partition, or
  tablespace clauses mixed into the same FK ALTER list.
- New public API or directory-layout behavior.

## Compatibility Impact

No SQL syntax change. The accepted SQL is MariaDB syntax already handled by the
embedded engine. The slice narrows ownerless crash-recovery classification for
one supported MariaDB ALTER shape.

## Directory, Lifecycle, And Native Storage Impact

No durable layout change. The covered ALTER shape is metadata-only from the
ownerless native file-operation perspective, but recovery retains the prearmed
native-dictionary checkpoint marker while a peer remains live. The marker
drains only after a recovery-capable no-live handoff.

## Public API, Build, Size, License

No public API, build-profile, dependency, binary-size, or license change.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selector `dictionary-foreign-key-mixed-comment-alter-crash`.
- Run adjacent FK dictionary crash CTests.
- Run production formatting and CI-production-build guards.
- Run `git diff --check`.

## Acceptance Criteria

- The mixed FK/comment ALTER reaches the existing `dictionary-before-finish`
  hook after native success.
- Live-peer recovery observes the recovered table comment and FK metadata.
- Dropped FK enforcement is absent while retained/added FK enforcement remains
  active.
- The native dictionary checkpoint marker stays set through live recovery and
  drains at the final recovery-capable boundary.
- Ownerless/native reopen and forced `.shm` rebuild preserve the recovered
  state.

## Risks And Follow-Up

- The classifier remains intentionally narrow. Broader mixed ALTER lists should
  be added as separate source-backed slices.
- Generated-column FK live recovery remains on the conservative native-file
  path until separately designed.
