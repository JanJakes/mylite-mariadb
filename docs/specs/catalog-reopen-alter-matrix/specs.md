# Catalog Reopen ALTER Matrix

## Problem

The catalog-reopen copy ALTER slice forced routed MyLite tables onto MariaDB's
copy algorithm before the in-place preparation path can write temporary `.frm`
files under a missing runtime schema directory. The first regression covered
the CHECK constraint path that exposed the bug.

That fix applies at the SQL-layer ALTER gate, so the next risk is coverage
depth rather than a different design: representative default-algorithm ALTER
families should prove they also work after catalog-only reopen, before the
roadmap stops listing filesystem-free schema DDL as a broad unknown.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_alter.cc:Sql_cmd_alter_table::execute()` delegates generic
  `ALTER TABLE` statements to `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc:is_inplace_alter_impossible()` is now the early
  MyLite gate that returns copy-only before in-place ALTER preparation.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` honors that early copy
  decision by setting the requested algorithm to
  `ALTER_TABLE_ALGORITHM_COPY`, while explicit no-copy algorithms still reject
  with a COPY requirement.
- `mariadb/sql/sql_parse.cc` handles standalone `SQLCOM_CREATE_INDEX` and
  `SQLCOM_DROP_INDEX` by calling `mysql_alter_table()` with prepared
  `Alter_info`.
- `mariadb/sql/sql_lex.h` parses standalone `CREATE INDEX` as
  `ALTER_ADD_INDEX`, and `mariadb/sql/sql_yacc.yy` parses standalone
  `DROP INDEX` as `ALTER_DROP_INDEX`.
- MariaDB documentation describes `ALTER TABLE` as the table-structure DDL
  surface with algorithm selection, and documents standalone `CREATE INDEX`
  and `DROP INDEX` as shortcuts that map to ALTER-backed index changes:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/alter/alter-table>,
  <https://mariadb.com/kb/en/create-index/>, and
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/drop/drop-index>.

## Design

Keep the implementation from the catalog-reopen copy ALTER slice. This slice
adds a regression matrix over the same path instead of adding another MyLite
DDL executor.

The storage-smoke coverage is folded into existing reopen tests to avoid
adding another full embedded server startup to the already long storage-smoke
process. The behavior under test is still catalog-only: the table is reopened
from the `.mylite` catalog and the runtime schema directory is asserted absent
before the representative ALTER statements run.

Covered reopened operations:

- default-algorithm `ALTER TABLE ... ADD COLUMN`,
- default-algorithm `ALTER TABLE ... DROP COLUMN`,
- default-algorithm `ALTER TABLE ... CHANGE COLUMN`,
- default-algorithm `ALTER TABLE ... ADD KEY`,
- default-algorithm `ALTER TABLE ... DROP INDEX`,
- default-algorithm standalone `CREATE INDEX`,
- default-algorithm standalone `DROP INDEX`,
- reopened `ALTER TABLE ... AUTO_INCREMENT = N`, and
- the existing reopened `ALTER TABLE ... DROP CONSTRAINT` CHECK regression.

## Supported Scope

- Representative default-algorithm copy ALTER operations on catalog-rediscovered
  routed MyLite tables.
- The covered table shapes are the current supported row, key, CHECK, and
  single-column autoincrement shapes.
- Existing explicit `ALGORITHM=COPY` same-runtime coverage remains in place.

## Non-Goals

- Exhaustive coverage for every MariaDB ALTER clause.
- In-place, instant, online, or metadata-only ALTER support.
- Failed ALTER rollback beyond the current statement-checkpoint coverage.
- Foreign-key, partition, FULLTEXT, SPATIAL, expression/hidden generated-index,
  generated-primary-key, or unsupported BLOB/TEXT index ALTER support.
- A separate standalone-index implementation outside MariaDB's ALTER path.

## Compatibility Impact

MyLite can now claim representative filesystem-free copy ALTER behavior after
catalog-only reopen for supported table shapes. The claim remains partial:
unsupported ALTER clauses still reject or remain planned, and explicitly
requested no-copy algorithms should still fail rather than silently changing
semantics.

## DDL Metadata Routing Impact

Successful covered ALTER statements publish replacement table-definition
metadata through the MyLite catalog and preserve one live catalog record for
the table after intermediate copy-rebuild cleanup.

## Single-File And Embedded-Lifecycle Impact

No durable sidecars are introduced. The regression keeps the runtime schema
directory absent before the reopened ALTER matrix and still requires the
runtime directory to be empty after close.

## Public API And File-Format Impact

No public `libmylite` API or file-format change is included.

## Storage-Engine Routing Impact

The behavior applies through the effective MyLite handler, so requested engines
such as `InnoDB`, `MyISAM`, and `Aria` continue to resolve to MyLite for the
supported routed table shapes.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol or integration-package changes are included.

## Binary-Size And Dependency Impact

No dependency is added and the embedded MariaDB profile does not change.

## Test And Verification Plan

- Extend storage-engine smoke coverage with reopened default-algorithm column,
  index, standalone-index, and autoincrement ALTER operations.
- Keep the existing reopened CHECK drop regression.
- Verify no runtime schema directory exists before the reopened ALTER matrix.
- Run format, tidy, first-party tests, embedded tests, storage-smoke tests, and
  routed DDL/CHECK/sidecar compatibility harness groups.

## Acceptance Criteria

- The reopened default-algorithm ALTER matrix succeeds on covered routed
  MyLite tables.
- Added columns expose default values and dropped columns become unavailable.
- Added indexes can serve forced-index reads and dropped indexes disappear
  from `SHOW INDEX`.
- Reopened standalone `CREATE INDEX` and `DROP INDEX` use the same
  catalog-backed rebuild lifecycle.
- Reopened `ALTER TABLE ... AUTO_INCREMENT = N` changes the next generated id.
- Docs distinguish representative reopened ALTER coverage from exhaustive ALTER
  compatibility.

## Risks And Unresolved Questions

- Runtime restart scalability in the storage-smoke binary is close to a
  MariaDB plugin-slot lifecycle limit; this slice avoids adding another restart,
  but repeated embedded init/end hardening should be handled separately.
- ALTER clauses outside the representative matrix may still contain earlier
  filesystem assumptions.
- SQL rollback and crash recovery for failed or interrupted copy ALTER remain
  broader transaction and recovery work.
