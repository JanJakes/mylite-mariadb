# Ownerless FK CHECK Mixed Alter Live Recovery

## Problem

Ownerless live-peer dictionary recovery accepts bounded mixed
`ALTER TABLE` lists that combine foreign-key add/drop clauses with selected
metadata-only or table-definition clauses. CHECK constraint ALTER clauses are
covered by standalone crash recovery, but they still are not accepted inside
the mixed FK/non-FK live-recovery parser. A writer death after MariaDB applies
`DROP FOREIGN KEY`, `DROP CONSTRAINT`, `ADD CONSTRAINT ... CHECK`, and
`ADD CONSTRAINT ... FOREIGN KEY` in one native statement can therefore leave a
live peer unable to classify the completed statement as recoverable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... ADD CONSTRAINT ... CHECK`
  and `ALTER TABLE ... DROP CONSTRAINT` as CHECK constraint ALTER clauses.
- `mariadb/sql/sql_table.cc` validates new CHECK expressions, resolves
  dropped CHECK names, and merges CHECK metadata into the rewritten table
  definition.
- `mariadb/sql/table.cc` enforces CHECK constraints through
  `TABLE::verify_constraints()`.
- `mariadb/sql/sql_show.cc` exposes CHECK metadata through
  `INFORMATION_SCHEMA.CHECK_CONSTRAINTS`.
- `packages/libmylite/src/database.cc`
  `ownerless_alter_table_mixed_foreign_key_recovery_statement()` already
  classifies bounded FK add/drop mixed lists and keeps generated-column FK
  tables excluded.

## Design

Extend the mixed foreign-key ALTER-list parser with two bounded CHECK clause
consumers:

- `ADD CONSTRAINT name CHECK (...)`
- `DROP CONSTRAINT name`

The mixed list still must include at least one FK add and one FK drop. FK-only
mixed lists keep the existing metadata-only FK recovery kind, supported column
mutations continue to select their stricter file-operation kind, and CHECK
clauses select a CHECK-specific file-operation recovery kind when no stricter
column kind is present.

This slice does not enable standalone CHECK live-peer recovery; it only accepts
CHECK clauses in the already-FK-classified mixed list where dictionary refresh
is already required for FK metadata. MariaDB's native table-definition work for
the focused CHECK shape can leave native file-operation evidence, so MyLite
retains the native file-operation checkpoint-needed marker while a peer remains
live and drains it during final no-live recovery.

## Compatibility Impact

No public API change. The SQL shape follows MariaDB syntax and preserves native
CHECK/FK enforcement after ownerless recovery.

## Directory And Native Storage Impact

No new files or formats are introduced. CHECK constraints are MariaDB table
definition metadata. The focused mixed CHECK/FK shape uses the conservative
native file-operation marker lane: the marker remains durable while a peer is
live, then drains after the final peer exits and no-live recovery checkpoints
the native state.

## Test Plan

- Add hook-build selector:
  `dictionary-foreign-key-mixed-check-alter-crash`.
- The selector creates a child table with FK A, FK B, and one table-level CHECK.
- Kill the writer after native success and before ownerless dictionary finish
  while another ownerless peer remains live.
- Verify live-peer recovery sees FK A removed, FK B and FK C enforced, the old
  CHECK removed, the new CHECK enforced, and the native file-operation marker
  retained.
- Verify ownerless/native reopen and forced `.shm` rebuild preserve the state.
- Verify final no-live recovery drains the native file-operation marker before
  ordinary native reopen.
- Run the focused selector, adjacent mixed-FK crash selectors, production build
  guard, ownerless-stress, format checks, and `git diff --check`.

## Acceptance Criteria

- Mixed FK plus CHECK ALTER lists are accepted only through the bounded parser,
  with duplicate CHECK adds and missing CHECK drops rejected before recovery
  classification.
- Live-peer recovery completes without waiting for no-live cleanup.
- CHECK and FK metadata/enforcement survive ownerless and ordinary native
  reopen before and after forced shared-memory rebuild.
- The native file-operation marker remains durable while a peer is live and is
  cleared by final no-live recovery.

## Risks And Follow-Up

- Standalone CHECK live-peer recovery remains a separate broader
  metadata-only DDL slice.
- Generated-column FK tables remain excluded by the existing mixed FK guards.
- Broader arbitrary mixed ALTER permutations and external randomized DDL stress
  remain planned.
