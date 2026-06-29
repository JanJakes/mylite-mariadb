# Ownerless Field Generated CHECK Live Recovery

## Problem Statement

Field-level and generated-column CHECK crash coverage previously proved the
no-live path: a writer killed after MariaDB completed the table-definition
mutation but before MyLite published dictionary finish could be recovered after
all peers closed. The remaining gap is live-peer recovery for the same bounded
CHECK shapes. A later ownerless opener must be able to finish the dead
dictionary generation while another ownerless peer remains open, and the
native file-operation marker must remain durable until final no-live drain.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:6195-6208` parses `CHECK (...)` expressions into
  `Virtual_column_info`.
- `mariadb/sql/sql_yacc.yy:6244-6250` attaches optional field-level CHECK
  expressions to parsed fields.
- `mariadb/sql/sql_yacc.yy:7999-8012` parses
  `ALTER TABLE ... MODIFY ... CHECK` and table-level CHECK ADD clauses.
- `mariadb/sql/sql_yacc.yy:8040-8049` parses `ALTER TABLE ... DROP CONSTRAINT`.
- `mariadb/sql/sql_table.cc:4034-4098` validates generated, default,
  field-level CHECK, and table-level CHECK expressions during table-definition
  processing.
- `mariadb/sql/sql_show.cc:7668-7708` exposes field-level and table-level
  CHECK metadata through `INFORMATION_SCHEMA.CHECK_CONSTRAINTS` with
  `LEVEL='Column'` or `LEVEL='Table'`.
- `mariadb/sql/table.cc:6616-6658` enforces CHECK constraints through
  `TABLE::verify_constraints()`.
- `packages/libmylite/src/database.cc` already uses
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_CHECK_CONSTRAINT` as the
  marker-retaining recovery kind for CHECK constraint ALTERs.

## Scope And Non-Goals

In scope:

- Classify the representative field-level CHECK ADD shape
  `MODIFY value INT NOT NULL CHECK (...)` when the column exists and the
  column-level CHECK metadata does not exist before execution.
- Classify the representative field-level CHECK removal shape
  `MODIFY value INT NOT NULL` when the column exists and the column-level CHECK
  metadata exists before execution.
- Continue classifying named table-level CHECK ADD/DROP clauses in the same
  comma-separated ALTER list with table-level metadata preconditions.
- Prove live-peer recovery, marker retention, marker drain, CHECK metadata,
  enforcement, ownerless/native reopen, and forced `.shm` rebuild for the
  existing field/generated CHECK ADD and DROP selectors.
- Register the selectors as focused hook CTests.

Out of scope:

- Arbitrary column ALTER grammar, generated-column creation/removal, and
  unrelated non-CHECK ALTER list members.
- Partitioned tables, `check_constraint_checks=OFF`, and concurrent CHECK DDL
  conflicts.
- Randomized CHECK DDL/RQG execution and SQL-level table-lock fault injection.

## Design

Extend the standalone CHECK recovery classifier in
`packages/libmylite/src/database.cc` with a narrow `MODIFY` clause consumer.
The consumer accepts a column definition tail only until the next top-level
comma or semicolon and rejects structural ALTER options that belong to broader
column/rebuild recovery classes. For ADD, it requires a top-level `CHECK`
token and proves the column-level constraint named after the column is absent.
For DROP, it requires no new CHECK expression and proves the column-level
constraint named after the column is present. Table-level CHECK clauses keep
using the existing table-level metadata helper.

The statement returns the existing
`MYLITE_OWNERLESS_DICTIONARY_RECOVERY_ALTER_TABLE_CHECK_CONSTRAINT` kind, so it
uses the existing native file-operation checkpoint marker lane. Tests kill the
writer at `dictionary-before-finish`, recover through a new ownerless opener
while a peer is still live, verify the marker remains set, release the peer,
and verify final no-live drain clears the marker.

## Compatibility Impact

No new SQL syntax or public API is enabled. The slice strengthens existing
ownerless CHECK compatibility evidence by proving completed MariaDB CHECK
metadata and enforcement state survive a writer death while another ownerless
process remains open.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native file format changes are introduced. CHECK
metadata remains MariaDB table-definition state for an InnoDB table inside the
MyLite database directory. The tests exercise ownerless process cleanup,
dictionary-generation recovery, native file-operation marker retention,
ordinary native reopen, and forced volatile `.shm` rebuild.

## Public API, Build, Size, License

No public API, dependency, license, or production binary-size impact beyond a
small statement-classifier helper and two focused hook CTest registrations.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selectors `dictionary-field-generated-check-crash` and
  `dictionary-field-generated-check-drop-crash`.
- Run the focused hook CTest filter for the two registered selectors and the
  adjacent standalone CHECK selectors.
- Run an adjacent mixed FK/CHECK hook CTest to verify parser ordering.
- Build the production embedded target and run the production
  `field-generated-check-ddl` selector.
- Run the production-build guard, formatter checks for touched sources,
  `git diff --check`, and cleanup checks.

## Acceptance Criteria

- Field/generated CHECK ADD and DROP statements are classified only for the
  bounded CHECK-related clauses whose metadata preconditions match the
  pre-execution table definition.
- A new ownerless opener recovers the completed CHECK ADD/DROP state while
  another ownerless peer remains live.
- The native file-operation checkpoint marker remains set while the peer is
  live and clears after final no-live recovery.
- Recovered CHECK ADD rejects invalid field-level and generated-column rows
  with MariaDB errno 4025 and accepts valid rows.
- Recovered CHECK DROP allows formerly invalid rows.
- Ownerless reopen, ordinary native reopen, and forced `.shm` rebuild preserve
  the recovered state.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Direct hook selectors `dictionary-field-generated-check-crash` and
  `dictionary-field-generated-check-drop-crash` passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-(field-generated-check(-drop)?|check-constraint(-drop)?)-crash$' --output-on-failure`
  passed 4/4 after formatting.
- Adjacent parser-ordering guard
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-foreign-key-mixed-check-alter-crash$' --output-on-failure`
  passed 1/1 after formatting.
- `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production selector `field-generated-check-ddl` passed after formatting.
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
  passed 1/1.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.
- Cleanup checks found no `mylite-ownerless-*` temp directories. A separate
  `/projects/mylite/build/dev/.../mylite_runtime_show_variables_optional_absence_test`
  process was present and left untouched because it did not belong to this
  worktree or ownerless selector.
- GitHub CI run `28351313453` for the previous head `b3729280d` had all
  native/build/PHPUnit shard jobs pass except
  `wordpress-phpunit-mysqli-mylite (phpunit-non-isolated-rest-short)`, which
  failed before PHPUnit execution because both Buildx setup attempts timed out
  pulling `moby/buildkit:buildx-stable-1` from Docker Hub. The final
  WordPress aggregate failed only because that shard result was `failure`.

## Risks And Follow-Up

- The classifier remains intentionally narrow and does not claim arbitrary
  non-CHECK ALTER lists.
- Broader metadata-only DDL, generated-column metadata changes, randomized DDL,
  and external MariaDB/RQG stress remain completion work.
