# CREATE OR REPLACE Foreign-Key LIKE And CTAS

## Goal

Broaden FK-aware `CREATE OR REPLACE TABLE` coverage beyond the plain
replacement form. Referenced parent replacement must fail for plain, `LIKE`,
and CTAS replacement forms, while child-table replacement must remove old FK
metadata for both `LIKE` and CTAS targets.

## Non-Goals

- Do not add new foreign-key action support.
- Do not cover temporary tables with foreign keys.
- Do not cover unsupported replacement definitions or partitioned tables.
- Do not add durable transactional DDL semantics beyond the existing statement
  checkpoint coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:4772-4822` handles base-table
  `CREATE OR REPLACE TABLE` by dropping the existing table before creating the
  replacement.
- `mariadb/sql/sql_table.cc:mysql_create_like_table()` preserves
  `OPT_OR_REPLACE`, so `CREATE OR REPLACE TABLE ... LIKE` reuses the same
  target drop lifecycle.
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` routes
  CTAS replacement through the no-lock create path after the old target has
  been dropped.
- `packages/mylite-storage/src/storage.c:mylite_storage_drop_table()` rejects
  dropping referenced parent tables and removes FK records owned by a dropped
  child table.
- `packages/libmylite/src/database.cc:mylite_exec()` wraps direct file-backed
  DDL in a statement checkpoint.

## Compatibility Impact

This narrows the remaining OR REPLACE gap for MyLite's supported FK subset:
referenced parents are protected regardless of the replacement form, and child
replacement cleanup is covered for representative `LIKE` and CTAS replacements.

## Affected MariaDB Subsystems

- SQL-layer `CREATE TABLE` / `LIKE` / CTAS replacement paths.
- MyLite handler table drop path.
- MyLite FK metadata storage and direct-SQL statement checkpointing.

## Design

No production change is expected unless coverage exposes a gap:

1. Build one referenced parent plus two child tables that both reference it.
2. Verify plain, `LIKE`, and CTAS `CREATE OR REPLACE TABLE parent` forms fail
   and preserve parent rows, child rows, catalog metadata, and FK metadata.
3. Replace one child via `CREATE OR REPLACE TABLE child LIKE template`.
4. Replace the other child via `CREATE OR REPLACE TABLE child (...) SELECT`.
5. Verify both old child FK records are gone, replacement definitions are
   visible, orphan child data can be inserted, and parent key changes are no
   longer blocked.
6. Close and reopen to verify catalog and FK metadata durability.

## DDL Metadata Routing Impact

The slice exercises existing catalog-backed DDL routing. It does not add a new
metadata record type.

## Single-File And Embedded Lifecycle

All durable state remains in the primary `.mylite` file. No persistent MariaDB
sidecars are allowed by the test.

## Public API Or File-Format Impact

No public C API or file-format change is introduced.

## Storage-Engine Routing Impact

The replacement tables continue to route through MyLite while preserving the
requested engine names used by the SQL definitions.

## Wire-Protocol Or Integration-Package Impact

None.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Extend storage-engine smoke coverage for FK-aware parent replacement failure
  across plain, `LIKE`, and CTAS forms.
- Cover child replacement cleanup for representative `LIKE` and CTAS targets.
- Run the focused storage-smoke test, `ctest --preset storage-smoke-dev`,
  `ctest --preset dev`, and `git diff --check`.

## Acceptance Criteria

- Parent replacement fails for plain, `LIKE`, and CTAS forms while preserving
  metadata and rows.
- Child `LIKE` and CTAS replacements remove old FK metadata.
- Replacement child tables remain visible across close/reopen.
- No durable sidecars appear.

## Implementation Status

Implemented in storage-engine smoke coverage. No production code change was
needed.

## Risks And Unresolved Questions

- Unsupported replacement definitions and lock-table interactions still need
  separate coverage.
- Temporary tables with foreign keys remain outside the supported FK subset.
