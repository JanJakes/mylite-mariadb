# Ownerless CHECK Constraint Live Recovery

## Problem Statement

Standalone CHECK constraint crash coverage previously proved only the
no-live recovery path: while another ownerless peer remained open, recovery
stayed busy until the peer exited. CHECK metadata is a bounded table-definition
DDL class, and the ownerless dictionary layer already has a marker-retaining
recovery kind for CHECK constraints when they appear inside mixed
foreign-key/CHECK ALTER lists. The standalone `ALTER TABLE ... ADD CONSTRAINT
... CHECK` and `ALTER TABLE ... DROP CONSTRAINT` shapes needed the same
live-peer recovery evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:7996-8011` parses table and column CHECK ADD
  clauses and records `ALTER_ADD_CHECK_CONSTRAINT`.
- `mariadb/sql/sql_yacc.yy:8044-8049` parses `DROP CONSTRAINT` as
  `Alter_drop::CHECK_CONSTRAINT` and records `ALTER_DROP_CHECK_CONSTRAINT`.
- `mariadb/sql/sql_table.cc:4034-4098` validates generated/default/CHECK
  expressions and table-level CHECK constraints during table-definition
  processing.
- `mariadb/sql/sql_table.cc:6438-6449` resolves CHECK constraint drops against
  the current table CHECK list.
- `mariadb/sql/sql_table.cc:11163-11223` rewrites ambiguous `DROP CONSTRAINT`
  requests that actually target foreign keys or unique keys and keeps real
  CHECK drops under `ALTER_DROP_CHECK_CONSTRAINT`.
- `mariadb/sql/table.cc:6616-6658` enforces field and table CHECK constraints
  through `TABLE::verify_constraints()`.
- `packages/libmylite/src/database.cc` already defines
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_CHECK_CONSTRAINT` as a
  recoverable dictionary state that forces the native file-operation
  checkpoint marker.

## Scope And Non-Goals

In scope:

- Recognize standalone CHECK ADD/DROP ALTER lists as the existing
  marker-retaining CHECK recovery kind.
- Keep duplicate CHECK ADD and missing CHECK DROP outside the live-recovery
  classifier by using pre-execution metadata checks.
- Convert the standalone CHECK ADD and CHECK DROP crash selectors to prove
  live-peer recovery, marker retention while the peer remains open,
  identity-sensitive final drain, ownerless/native reopen, and forced `.shm`
  rebuild.
- Register both selectors as focused hook CTests.

Out of scope:

- Generated-column CHECK live-peer recovery. Existing generated/field CHECK
  crash selectors remain conservative no-live evidence.
- Arbitrary mixed non-FK ALTER permutations.
- `check_constraint_checks=OFF`, partitioned tables, and randomized external
  DDL/RQG execution.
- SQL-level local table-lock fault injection.

## Design

Add `ownerless_alter_table_check_constraint_recovery_statement()` in
`packages/libmylite/src/database.cc`. It accepts only `ALTER TABLE <table>`
lists whose comma-separated clauses are all:

- `ADD CONSTRAINT <name> CHECK (...)`, where the named CHECK constraint is not
  already present before execution; or
- `DROP CONSTRAINT <name>`, where the named CHECK constraint is present before
  execution.

The classifier returns the existing
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_CHECK_CONSTRAINT` kind. That
kind remains on the native file-operation marker lane, so the marker stays
durable while a peer is live. A final older survivor preserves it when the
ALTER changed the file-per-table identity; metadata-only cases with unchanged
identity may drain it as that final peer closes.

## Compatibility Impact

No new SQL syntax or public API is enabled. The slice strengthens existing
CHECK constraint compatibility by allowing a later ownerless process to finish
dictionary recovery while another ownerless peer remains open after a writer
dies between MariaDB's completed CHECK metadata mutation and MyLite dictionary
finish publication.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native file format changes are introduced. CHECK
metadata remains MariaDB table-definition state for an InnoDB table inside the
MyLite database directory. The tests prove recovered metadata and enforcement
survive ownerless reopen, ordinary native reopen, and forced shared-memory
rebuild. The native file-operation checkpoint marker drains at final no-live
shutdown when the file-per-table identity is unchanged, or after the following
isolated recovery when an older survivor observed an identity change.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact beyond a
small statement classifier branch in existing first-party ownerless recovery
code.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selectors `dictionary-check-constraint-crash` and
  `dictionary-check-constraint-drop-crash`.
- Run the focused hook CTest filter for both registered standalone CHECK
  crash selectors.
- Run the adjacent mixed FK/CHECK hook CTest to verify parser ordering remains
  correct.
- Build the production embedded target and run a production ownerless selector
  that exercises CHECK constraint refresh without unsafe hooks.
- Run production-build guard, formatter/static checks for touched sources,
  `git diff --check`, and cleanup checks.

## Acceptance Criteria

- Standalone CHECK ADD and CHECK DROP statements are classified as recoverable
  only for bounded CHECK clauses whose metadata preconditions match the
  pre-execution table definition.
- A new ownerless opener recovers CHECK metadata while another ownerless peer
  remains live after the writer is killed.
- The native file-operation checkpoint marker remains set while the peer is
  live. CHECK DROP drains it at final no-live shutdown; a CHECK ADD rebuild
  whose tablespace identity changed preserves it through the older survivor
  and clears after isolated latest-generation recovery.
- Recovered CHECK ADD rejects invalid rows with MariaDB errno 4025 and accepts
  valid rows.
- Recovered CHECK DROP allows formerly invalid rows.
- Ownerless reopen, ordinary native reopen, and forced `.shm` rebuild preserve
  the recovered state.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Direct hook selectors `dictionary-check-constraint-crash` and
  `dictionary-check-constraint-drop-crash` passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-check-constraint(-drop)?-crash$' --output-on-failure`
  passed 2/2.
- Adjacent parser-ordering guard
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-foreign-key-mixed-check-alter-crash$' --output-on-failure`
  passed 1/1.
- `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production selector `check-constraint-ddl` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
  passed 1/1.
- Ubuntu 24.04 `clang-format --dry-run --Werror` passed for
  `packages/libmylite/src/database.cc` and
  `packages/libmylite/tests/ownerless_cross_process_sql_test.c`.
- `git diff --check` passed.
- Cleanup checks found no `mylite-ownerless-*` temp directories and no
  lingering MyLite or MariaDB test processes.

## Risks And Follow-Up

- The classifier intentionally stays narrow. It does not claim arbitrary
  non-FK mixed ALTER lists or generated-column CHECK live-peer recovery.
- Broader metadata-only DDL, generated-column metadata-only recovery, external
  MariaDB/RQG execution, and randomized CHECK DDL crash variants remain
  completion work.
