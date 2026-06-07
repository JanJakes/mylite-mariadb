# Ownerless Pressure Generated-Column FK Policy

## Problem

Ownerless active-reader pressure coverage now blocks generated-column ALTER and
generated-column secondary-index DDL while retained page-version WAL is at the
configured soft limit. The remaining generated-column pressure gap is the
supported intersection with foreign-key DDL: stored generated child columns and
stored generated referenced columns exercise MariaDB's generated-column key
validation plus InnoDB's native foreign-key dictionary machinery.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_TABLE` with
  `CF_CHANGES_DATA`, so `ALTER TABLE ... ADD CONSTRAINT` and
  `ALTER TABLE ... DROP FOREIGN KEY` are write statements in MariaDB's command
  model.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table()` handles
  `Key::FOREIGN_KEY` key parts with `key_add_part_check_null()` and
  `Column_definition::check_vcol_for_key()`, so generated-column FK DDL crosses
  distinct generated-column key validation before native execution.
- `mariadb/sql/field.cc:Column_definition::check_vcol_for_key()` rejects
  nondeterministic generated expressions used by key definitions.
- `mariadb/storage/innobase/handler/ha_innodb.cc:
  create_table_info_t::create_foreign_keys()` resolves child and referenced
  InnoDB indexes, records action rules, rejects unsupported generated-column
  action combinations through native dictionary checks, and writes FK metadata
  into the InnoDB dictionary.
- `packages/libmylite/src/database.cc:
  enforce_ownerless_page_log_limit_policy()` runs before ownerless statement
  locks, dictionary refresh, and MariaDB execution. The pressure test must prove
  generated-column FK DDL returns `MYLITE_BUSY` before native metadata changes
  when retained WAL is already at the configured limit.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-write-policy` with:
  - `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` over a stored generated
    child column, and
  - `ALTER TABLE ... DROP FOREIGN KEY` from a constraint referencing a stored
    generated parent column.
- Verify those statements return `MYLITE_BUSY` while a live snapshot pin keeps
  page-version WAL at the configured limit.
- Verify blocked statements leave generated-column FK metadata unchanged.
- Verify the same statements succeed after the reader releases, including
  generated child-column FK enforcement and orphan inserts after the referenced
  generated-column FK is dropped.
- Verify final metadata and rows survive ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Production pressure classifier changes.
- Virtual generated child foreign keys, MariaDB-rejected generated-column
  action clauses, exhaustive generated-column FK matrices, or FK action crash
  fault selection.
- SQL-level table-lock fault injection.
- External MariaDB/RQG randomized pressure stress.

## Design

Reuse the retained-WAL setup from `active-reader-pressure-write-policy`:

1. Create a regular parent table and a child table with a stored generated
   `parent_key`, indexed but without a foreign key.
2. Create a second table pair where the parent has a stored generated unique
   `parent_key` and the child starts with a regular foreign key referencing it.
3. Hold a repeatable-read snapshot in a peer ownerless process.
4. Commit one ownerless update so page-version WAL remains retained by the
   reader pin.
5. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
6. Assert generated-child FK ADD and generated-referenced FK DROP return
   `MYLITE_BUSY`.
7. Assert blocked state has not added or removed the generated-column FK
   metadata and that generated values remain unchanged.
8. Release the reader, execute the same ADD/DROP statements successfully, then
   verify valid generated-child FK inserts, missing-parent rejection, and
   orphan inserts after the referenced generated-column FK drop.
9. Verify final rows and constraint metadata through ownerless/native reopen
   before and after forced shared-memory rebuild.

## Compatibility Impact

No SQL semantics change. The slice adds deterministic evidence that supported
stored generated-column FK DDL is pressure-throttled before MariaDB mutates
native metadata while retained page-version WAL is over the configured
ownerless limit.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises existing InnoDB table
metadata/files, ownerless dictionary generation, page-version WAL retention,
checkpointing, and forced shared-memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked statements must not reach native
generated-column FK metadata mutation paths. After pressure clears, MariaDB's
native ALTER TABLE and InnoDB foreign-key dictionary machinery remain
responsible for stored generated-column FK metadata and enforcement.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `active-reader-pressure-write-policy` selector in
  `embedded-dev`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run the ownerless SQL CTest shard containing the selector in both presets.
- Run adjacent active-reader pressure stress coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Generated-column FK ADD and DROP DDL return `MYLITE_BUSY` with the pressure
  limit diagnostic while retained WAL is at the configured limit.
- Blocked statements leave generated-column FK metadata unchanged.
- After the reader releases, the same statements succeed, generated-column FK
  enforcement and post-drop orphan inserts behave correctly, and final state
  survives ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is deterministic generated-column FK pressure coverage, not exhaustive
  generated-column FK compatibility.
- Virtual generated child foreign keys, MariaDB-rejected generated-column FK
  action clauses, full FK action crash fuzzing, and external randomized
  pressure stress remain separate work.
