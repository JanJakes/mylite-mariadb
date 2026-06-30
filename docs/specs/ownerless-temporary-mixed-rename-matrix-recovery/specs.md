# Ownerless Temporary Mixed Rename Matrix Recovery

## Problem Statement

Ownerless temporary/permanent `RENAME TABLE IF EXISTS` coverage already proves
focused temporary-first and permanent-first lists, plus cross-schema rollback
after one durable native rename. The remaining documented gap is broader order
coverage where temporary renames, missing-source no-ops, and multiple permanent
file-per-table renames share one statement while another ownerless peer stays
live.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_RENAME_TABLE` to
  `mysql_rename_tables(thd, first_table, 0, lex->if_exists())`.
- `mariadb/sql/temporary_tables.cc:581-606` implements
  `THD::rename_temporary_table()` for connection-local temporary table list
  entries.
- `mariadb/sql/sql_table.cc` records durable rename work in the DDL log and
  rolls native file operations back when a rename list fails or is interrupted.
- MyLite's hook tests can stop at `rename-table-after-native-file-op`, and the
  existing fault helper can skip earlier occurrences to kill after a later
  native file operation.

## Scope And Non-Goals

In scope:

- Add a focused cross-schema `RENAME TABLE IF EXISTS` matrix with:
  - a missing-source no-op before durable work,
  - two permanent file-per-table cross-schema renames,
  - one connection-local temporary rename between the durable renames, and
  - missing-source no-ops between and after the durable renames.
- Kill the writer after the first durable native file operation.
- Kill the writer after the second durable native file operation.
- Keep a live ownerless peer open during recovery and verify final ownerless,
  native, and forced-`.shm` reopen state.

Out of scope:

- Random SQL generation.
- Product behavior changes beyond exposing the existing recovery path to more
  permutations.
- SQL-level local table-wait positive fault injection.

## Design

The new selectors reuse the ownerless dictionary crash harness. The writer
creates a temporary table shadowing a durable table, then executes a mixed
`RENAME TABLE IF EXISTS` list with two permanent cross-schema renames and one
temporary rename. One selector stops at the first
`rename-table-after-native-file-op` hook. The second selector uses the existing
fault-skip mechanism to stop at the second durable native file operation.

Recovery assertions verify that MariaDB's DDL-log rollback and MyLite's
ownerless dictionary cleanup leave the durable sources in place, remove all
durable targets, preserve original InnoDB space ids, retain the native
file-operation checkpoint marker while a live peer remains open, and clear it
after the final live peer closes and the checkpoint drain completes.

## Compatibility Impact

SQL behavior remains MariaDB-compatible. Missing-source pairs under
`RENAME TABLE IF EXISTS` remain warning/no-op pairs, temporary renames remain
connection-local, and permanent table files stay in MariaDB native formats
inside the MyLite database directory.

## Directory And Native Storage Impact

No directory layout or native format changes. The tests prove that interrupted
cross-schema file-per-table rename operations roll back without leaving `.frm`
or `.ibd` targets behind, and that a forced shared-memory rebuild uses durable
native state rather than volatile ownerless state as truth.

## Public API, Build, Size, And License

No public API, dependency, license, or production binary-size impact. The slice
adds hook-only test selectors, CTest registrations, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the two new CTest selectors.
- Run the adjacent temporary mixed rename CTest subset.
- Run the focused ownerless hook crash subset if time permits.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- A writer killed after the first durable native file operation leaves both
  permanent source tables and the shadowing permanent table durable and
  writable while a live peer remains open.
- A writer killed after the second durable native file operation rolls back
  both permanent renames, leaves no temporary or missing-source targets in the
  durable dictionary, and preserves both original InnoDB space ids.
- The native file-operation checkpoint marker remains while the live peer is
  open and drains after final close.
- Ownerless reopen, native reopen, and forced-`.shm` rebuild observe the same
  durable state.

## Verification Results

Completed on 2026-06-30 with `ownerless-test-hooks`, `embedded-prod`, and
`prod` formatting guards:

- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j8`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test temporary-cross-schema-mixed-if-exists-matrix-loop-crash`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test temporary-cross-schema-mixed-if-exists-matrix-second-loop-crash`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-temporary-cross-schema-mixed-if-exists-matrix.*loop-crash$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-temporary-(mixed-if-exists|cross-schema-mixed-if-exists).*(rename|matrix).*crash$' --output-on-failure`
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-temporary-.*crash$' --output-on-failure`
- `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test -j8`
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `git diff --check`

## Risks And Follow-Up

- This is still deterministic matrix coverage, not external MariaDB/RQG
  randomization.
- It does not claim support for SQL-level ownerless `LOCK TABLES`.
