# Primary-Key ALTER DDL

## Goal

Cover the routed primary-key ALTER lifecycle for supported MyLite tables:
adding a primary key through non-CHECK constraint syntax, skipping duplicate
`ADD PRIMARY KEY IF NOT EXISTS`, dropping the primary key, accepting duplicate
values after the drop, rejecting a re-add over incompatible duplicate rows, and
successfully re-adding the primary key after the data is made compatible.

## Non-Goals

- Foreign-key metadata or enforcement.
- Unsupported key classes such as FULLTEXT, SPATIAL, vector, or unbounded
  long-unique BLOB/TEXT keys.
- Online, in-place, instant, or no-copy primary-key ALTER algorithms.
- Exhaustive primary-key syntax permutations, composite autoincrement
  allocation semantics, or transaction/savepoint rollback.
- Preserving user-provided primary-key constraint names. MariaDB exposes the
  primary index as `PRIMARY`.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB `ALTER TABLE` documentation lists
  `ADD [CONSTRAINT [symbol]] PRIMARY KEY [IF NOT EXISTS] (...)` and
  `DROP [CONSTRAINT] PRIMARY KEY`: <https://mariadb.com/kb/en/alter-table/>.
- `mariadb/sql/sql_yacc.yy:7236-7238` maps primary-key constraint syntax to
  `Key::PRIMARY`.
- `mariadb/sql/sql_yacc.yy:8005-8012` parses `ALTER TABLE ... ADD
  constraint_def` into the normal ALTER key list.
- `mariadb/sql/sql_yacc.yy:8060-8070` parses `DROP [CONSTRAINT] PRIMARY KEY`
  into `Alter_drop::KEY` with the canonical `PRIMARY` key name.
- `mariadb/sql/sql_table.cc:6375-6465` handles DROP KEY `IF EXISTS` filtering
  for existing table keys before ALTER execution.
- `mariadb/sql/sql_table.cc:6860-6910` forces rebuilds when old and new primary
  key state differs.
- `mariadb/sql/sql_table.cc:9088-9130` removes keys listed in the ALTER drop
  list and marks primary-key changes as table-definition changes.

## Compatibility Impact

This moves a bounded part of the broader non-CHECK constraint matrix from
planned to covered:

- supported `ALTER TABLE ... ADD CONSTRAINT ... PRIMARY KEY` through copy
  rebuild;
- duplicate `ADD PRIMARY KEY IF NOT EXISTS` warning behavior;
- `ALTER TABLE ... DROP PRIMARY KEY` through copy rebuild;
- failed re-add over duplicate existing rows preserving the non-primary table;
- successful re-add after cleanup, including close/reopen discovery.

Foreign keys, unsupported key classes, broader syntax matrices, online DDL, and
full SQL transaction semantics remain planned or explicitly unsupported.

## Design

No storage-format or handler change is expected. MyLite already maintains
primary-key index entries and copy ALTER rebuilds. The slice should prove the
MariaDB primary-key ALTER forms arrive at existing MyLite row/index paths:

1. Create a routed `ENGINE=InnoDB` table without a primary key.
2. Add a primary key with `ALTER TABLE ... ADD CONSTRAINT ... PRIMARY KEY,
   ALGORITHM=COPY`.
3. Verify `SHOW INDEX`, duplicate rejection, and forced primary-key reads.
4. Exercise duplicate `ADD PRIMARY KEY IF NOT EXISTS` warning semantics.
5. Drop the primary key with `ALTER TABLE ... DROP PRIMARY KEY,
   ALGORITHM=COPY`.
6. Verify the primary index is gone, primary-key hints fail, and duplicate ids
   can be inserted.
7. Attempt to re-add the primary key while duplicate rows exist and verify the
   old non-primary table remains visible.
8. Remove the duplicate row, re-add the primary key, and verify persistence
   after close/reopen.

## File Lifecycle

The covered ALTER statements use MyLite's primary file and rollback journal
publication path. They must not leave persistent `.frm`, `.ibd`, `.MYD`,
`.MYI`, `.MAI`, `.MAD`, `aria_log.*`, binlog, relay-log, or plugin-owned table
files. Old row, index, and definition pages remain unreclaimed until compaction
exists.

## Embedded Lifecycle And API

No public `libmylite` API changes. Direct SQL execution should expose MariaDB
success, warning, and duplicate-key diagnostics through the existing APIs.

## Build, Size, And Dependencies

No new dependency or build-profile change. Binary-size impact is limited to
test code unless a bug fix is required.

## Test Plan

- Add storage-smoke coverage for primary-key add/drop/re-add on a routed
  `ENGINE=InnoDB` table.
- Verify catalog metadata, `SHOW INDEX`, duplicate-key behavior, forced-index
  behavior, failed re-add rollback, close/reopen visibility, and sidecar gates.
- Update compatibility, roadmap, harness, and non-CHECK constraint docs.
- Run focused storage-smoke, routed DDL/DML and sidecar reports, format and
  diff checks, shell checks, tidy, and full `dev`, `embedded-dev`, and
  `storage-smoke-dev` gates.

## Acceptance Criteria

- `ALTER TABLE ... ADD CONSTRAINT ... PRIMARY KEY, ALGORITHM=COPY` creates a
  maintained primary key on a supported routed table.
- Duplicate `ADD PRIMARY KEY IF NOT EXISTS` warns without mutating table state.
- `ALTER TABLE ... DROP PRIMARY KEY, ALGORITHM=COPY` removes the maintained
  primary key while preserving rows and other supported indexes.
- Re-adding the primary key over duplicate existing rows fails without losing
  table visibility or the duplicate data.
- Re-adding after cleanup persists across close/reopen without durable MariaDB
  sidecars.

## Implementation Status

Implemented in storage-smoke coverage:

- A routed `ENGINE=InnoDB` table without a primary key gains a maintained
  primary key through `ALTER TABLE ... ADD CONSTRAINT ... PRIMARY KEY,
  ALGORITHM=COPY`.
- Duplicate `ADD PRIMARY KEY IF NOT EXISTS` succeeds with MariaDB's
  `Multiple primary key defined` warning while preserving the existing primary
  key.
- `ALTER TABLE ... DROP PRIMARY KEY, ALGORITHM=COPY` removes the primary key,
  rejects primary-key hints, preserves the secondary index, and allows duplicate
  id values.
- Re-adding the primary key over duplicate rows fails without losing the table
  or duplicate data.
- Re-adding after deleting the duplicate row restores maintained primary-key
  enforcement and survives close/reopen without durable sidecars.

## Risks And Open Questions

- Primary-key name handling remains MariaDB-owned; MyLite should assert
  `PRIMARY`, not a user-supplied constraint name.
- This does not claim transactional DDL semantics beyond the existing statement
  checkpoint coverage used by the storage-smoke path.
- Composite primary-key coverage exists elsewhere for application schemas, but
  composite autoincrement semantics remain planned.
