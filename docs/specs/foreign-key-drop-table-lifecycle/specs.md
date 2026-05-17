# Foreign-Key DROP TABLE Lifecycle

## Goal

Cover SQL-level `DROP TABLE` behavior for MyLite-routed tables that participate
in the supported public foreign-key subset. Dropping a referenced parent table
must fail without publishing partial catalog changes, while dropping a child
table must remove its FK metadata so the parent can be dropped afterward.

## Non-Goals

- Do not add cascading actions, `SET NULL`, `SET DEFAULT`, deferred checks, or
  multi-row FK ordering.
- Do not make `foreign_key_checks=0` relax parent-table `DROP TABLE`
  restrictions.
- Do not physically reclaim orphaned table-definition, row, index-entry, or FK
  blob pages.
- Do not add transactional DDL inside user-managed transactions.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:mysql_rm_table()` locks `DROP TABLE` targets and
  delegates base-table deletion through `mysql_rm_table_no_locks()`.
- `mariadb/sql/sql_table.cc:1690-1709` records DDL-log state and calls
  `ha_delete_table()` for each base table being dropped.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_table()` maps
  MariaDB's canonical table path to a schema/table name and calls
  `mylite_storage_drop_table()` for durable MyLite-routed base tables.
- `packages/mylite-storage/src/storage.c:mylite_storage_drop_table()` removes
  child FK catalog records before removing the child table, rejects dropping a
  referenced parent while parent-listable FK records remain, and writes the
  updated catalog only after all checks pass.
- `docs/specs/foreign-key-storage-metadata/specs.md` already requires
  storage-layer referenced-parent drop rejection and child-table FK cleanup.
  This slice lifts that coverage through the SQL handler path.

## Compatibility Impact

This is partial InnoDB-compatible FK lifecycle behavior for MyLite's supported
`RESTRICT` / `NO ACTION` subset. A referenced parent table remains protected by
FK metadata. A child table can be dropped, and the child FK metadata disappears
with the table. After the child table is gone, the formerly referenced parent
can be dropped normally.

The current MyLite behavior remains stricter than full InnoDB import workflows:
`foreign_key_checks=0` does not intentionally create persistent FK metadata
that references a missing parent table.

## Design

No new production path is expected. The existing SQL and storage flow should be
sufficient:

1. MariaDB routes `DROP TABLE` to the MyLite handler through `ha_delete_table()`.
2. `ha_mylite::delete_table()` calls `mylite_storage_drop_table()`.
3. The storage layer rejects referenced-parent drops before publishing the
   rebuilt catalog page.
4. Child-table drops remove child FK records and the table record in the same
   catalog publication.

If SQL-level coverage exposes a partial publication bug, fix the storage or
handler path rather than weakening the compatibility claim.

## File Lifecycle

The lifecycle stays inside the primary `.mylite` file. No persistent `.frm`,
`.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`, binlog, relay-log, or
plugin-owned durable sidecar is introduced. Dropped FK blob pages may remain
orphaned until a future compaction slice.

## Embedded Lifecycle And API

No public C API changes are required. Direct SQL execution exposes the behavior
through existing `mylite_exec()` diagnostics and close/reopen discovery.

## Build, Size, And Dependencies

No dependency or size-profile change is intended. The test uses the existing
storage-smoke embedded build.

## Test Plan

- Create a routed InnoDB parent/child FK pair under `libmylite`.
- Verify a referenced parent `DROP TABLE` fails while preserving parent rows,
  child rows, table catalog records, and FK information-schema metadata.
- Drop the child table and verify the child table record and FK metadata are
  gone.
- Drop the formerly referenced parent table.
- Close and reopen the database and verify both tables remain absent.
- Run the focused storage-smoke test, storage-smoke CTest preset, default CTest
  preset, format check, and `git diff --check`.

## Acceptance Criteria

- SQL-level referenced-parent `DROP TABLE` fails for a live supported FK.
- Failed parent drop preserves the original catalog and row visibility.
- SQL-level child `DROP TABLE` removes child FK metadata.
- Parent `DROP TABLE` succeeds after child-table cleanup.
- Close/reopen discovery keeps both dropped tables absent.
- Docs and compatibility tables no longer describe this behavior as
  storage-layer-only coverage.

## Risks And Open Questions

- Multi-table `DROP TABLE parent, child` ordering remains outside this slice.
- Full InnoDB behavior around `foreign_key_checks=0` and parent drops needs a
  separate compatibility decision before MyLite allows missing-parent FK
  metadata.
- Physical page reclamation remains part of future free-space and compaction
  work.
