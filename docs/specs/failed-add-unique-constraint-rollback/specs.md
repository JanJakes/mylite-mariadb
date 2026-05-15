# Failed ADD UNIQUE Constraint Rollback

## Goal

Cover failed `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE` over existing duplicate
rows on a MyLite-routed table. The failed copy rebuild must preserve the old
table definition, rows, supported indexes, catalog metadata, and close/reopen
visibility. After duplicate cleanup, the same unique constraint should add
successfully and enforce duplicates.

## Non-Goals

- Foreign-key metadata or enforcement.
- Unsupported key classes such as FULLTEXT, SPATIAL, vector, or unbounded
  long-unique BLOB/TEXT keys.
- Online, in-place, instant, or no-copy unique-key ALTER algorithms.
- Exhaustive unique-constraint syntax, nullable unique-key matrices, or
  transaction/savepoint rollback.
- DML `INSERT ... ON DUPLICATE KEY` or CTAS duplicate-mode behavior, which is
  covered by separate row/CTAS slices.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB `ALTER TABLE` documentation lists
  `ADD [CONSTRAINT [symbol]] UNIQUE [INDEX|KEY] [IF NOT EXISTS] ...` and notes
  duplicate existing values cause an error unless `IGNORE` is used:
  <https://mariadb.com/kb/en/alter-table/>.
- `mariadb/sql/sql_yacc.yy:7236-7238` maps `UNIQUE [KEY|INDEX]` constraint
  syntax to `Key::UNIQUE`.
- `mariadb/sql/sql_yacc.yy:8005-8012` parses `ALTER TABLE ... ADD
  constraint_def` into the normal ALTER key list.
- `mariadb/sql/sql_table.cc:6860-6910` detects key changes that require rebuilds
  when the old and new table definitions differ.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` drives the copy ALTER path
  that creates the rebuilt table, copies existing rows, and aborts on handler
  duplicate-key errors from the new unique key.
- `mariadb/storage/mylite/ha_mylite.cc` routes copy-rebuild writes through
  MyLite's duplicate-key checks and table-definition publication path.

## Compatibility Impact

This moves another bounded non-CHECK constraint rollback case from planned to
covered:

- failed `ADD CONSTRAINT ... UNIQUE` over duplicate existing rows preserves the
  pre-statement table;
- existing secondary indexes remain usable after the failed ALTER;
- the unique constraint can be added successfully after duplicate cleanup and
  survives close/reopen.

Broader unique-key matrices, nullable edge cases, online DDL, foreign keys, and
full SQL transaction/savepoint semantics remain planned.

## Design

No handler change is expected. MyLite's statement checkpoint should protect the
old table while MariaDB's copy ALTER attempts to populate the rebuilt table:

1. Create a routed `ENGINE=InnoDB` table with duplicate values in a non-unique
   column and a supported secondary index.
2. Attempt `ALTER TABLE ... ADD CONSTRAINT author_unique UNIQUE (author),
   ALGORITHM=COPY`.
3. Verify the statement fails, the old table remains visible, the unique index
   is absent, duplicates remain, and the secondary index still works.
4. Close/reopen and repeat the visibility checks.
5. Delete the duplicate row, add the unique constraint successfully, and verify
   duplicate rejection plus forced-index reads before and after close/reopen.

## File Lifecycle

The failed and successful ALTER statements may use MyLite's rollback journal.
They must not leave persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`,
`aria_log.*`, binlog, relay-log, or plugin-owned table files. Old pages remain
unreclaimed until compaction exists.

## Embedded Lifecycle And API

No public `libmylite` API changes. Existing direct SQL diagnostics expose the
duplicate-key failure.

## Build, Size, And Dependencies

No new dependency or build-profile change. Binary-size impact is limited to
test code unless a bug fix is required.

## Test Plan

- Add storage-smoke coverage for failed unique-constraint add over duplicate
  existing rows.
- Verify catalog metadata, absent/present `SHOW INDEX` rows, duplicate-row
  preservation, forced secondary-index reads, close/reopen visibility, and
  sidecar gates.
- Update compatibility, roadmap, storage architecture, harness, and non-CHECK
  constraint docs.
- Run focused storage-smoke, routed DDL/DML and sidecar reports, format and
  diff checks, shell checks, tidy, and full `dev`, `embedded-dev`, and
  `storage-smoke-dev` gates.

## Acceptance Criteria

- Failed `ADD CONSTRAINT ... UNIQUE` over duplicate existing rows does not make
  a partial unique key visible.
- The old table rows and supported indexes remain usable before and after
  close/reopen.
- The same unique constraint succeeds after duplicate cleanup and enforces
  duplicate-key rejection before and after close/reopen.
- Durable sidecar gates pass.

## Implementation Status

Implemented in storage-smoke coverage:

- A routed `ENGINE=InnoDB` table with duplicate `author` values rejects
  `ALTER TABLE ... ADD CONSTRAINT author_unique UNIQUE (author),
  ALGORITHM=COPY`.
- The failed ALTER leaves `author_unique` absent, keeps duplicate rows visible,
  and preserves the existing secondary index before and after close/reopen.
- After deleting the duplicate row, the same unique constraint adds
  successfully, rejects duplicate inserts, supports forced-index reads, and
  survives close/reopen without durable sidecars.

## Risks And Open Questions

- This proves statement-level preservation for the covered copy ALTER path, not
  full SQL transaction or savepoint rollback.
- Nullable unique-key behavior is already covered elsewhere for initial keys;
  this slice does not expand nullable ALTER matrices.
