# Keyless Update/Delete Row Lifecycle

## Problem

MyLite can append and full-scan rows, but `UPDATE` and `DELETE` still inherit
the base handler's unsupported behavior. Real application DML needs at least a
bounded keyless table path before compatibility harness and application-schema
work can move beyond insert/select smoke coverage.

Supporting update/delete is not only a handler method problem. MariaDB's
handler contract expects engines to identify the current row via `position()`
and sometimes fetch it later with `rnd_pos()`, especially when statements use
sorting or delayed positioning. MyLite's current rowset only returns payload
bytes, so it has no durable row identity to update or delete.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h:4375-4410` makes `rnd_next()`, `rnd_pos()`, and
  `position()` the random-scan positioning contract.
- `mariadb/sql/handler.h:5238-5267` leaves `write_row()`, `update_row()`, and
  `delete_row()` unsupported until a handler overrides them.
- `mariadb/storage/heap/ha_heap.cc:409-421` stores an engine-native pointer in
  `ref` from `position()` and reads it back in `rnd_pos()`.
- `mariadb/storage/csv/ha_tina.cc:1279-1307` documents that `position()` can be
  called after each `rnd_next()` and that sorted statements can later reposition
  rows through `rnd_pos()`.
- `mariadb/storage/csv/ha_tina.cc:1088-1151` implements update/delete by
  marking the current scanned row and rebuilding through a temporary file. That
  confirms the current row is the target for table-scan updates/deletes, but
  its file rewrite model is not MyLite's final storage model.

## Design

Add a narrow, append-only keyless row lifecycle:

- Treat the row page id as the durable row id for the row payload stored in
  that page.
- Extend `mylite_storage_rowset` with row ids so the handler can map scan rows
  back to primary-file row pages.
- Add a row-state page type keyed by catalog table id and source row id:
  - delete state marks a row id as no longer visible;
  - replace state marks a row id as superseded by a newly appended row page.
- `mylite_storage_delete_row()` appends a delete state page for a live row id.
- `mylite_storage_update_row()` appends a replacement row payload, then appends
  a replace state page linking the old row id to the new row id.
- Full scans build the latest row-state map before returning rows:
  - base row pages with no state remain visible;
  - deleted row ids are hidden;
  - replaced row ids are hidden and the replacement row page is visible unless
    it is later deleted or replaced.
- `ha_mylite::position()` stores the current row id in `ref`.
- `ha_mylite::rnd_pos()` reads a row id from `ref` and fetches that live row.
- `ha_mylite::update_row()` and `ha_mylite::delete_row()` use the current row
  id, serialize replacement payloads through the existing row serializer, and
  call the storage lifecycle APIs.

This is deliberately not a transaction implementation. State pages are
append-only and visible after the header page count is advanced. Later recovery
work must define atomic publication and rollback.

## Scope

This slice supports keyless MyLite-routed base tables only:

- tables without declared keys,
- nullable fixed and variable fields,
- BLOB/TEXT payload rows,
- full-scan `UPDATE` and `DELETE`,
- close/reopen visibility,
- `rnd_pos()` for live keyless rows.

## Non-Goals

- Updates/deletes for primary-key, unique-key, secondary-index, or
  autoincrement-key tables.
- Duplicate-key checks on updated rows.
- Transaction rollback, statement rollback, savepoints, or crash recovery.
- In-place page rewrite or free-space reclamation.
- Multi-row atomicity beyond the current append-only smoke behavior.
- `TRUNCATE TABLE`, copy `ALTER`, or index maintenance.

## Compatibility Impact

Keyless `UPDATE` and `DELETE` move from planned to partial. MyLite still cannot
claim general MySQL/MariaDB DML compatibility because keyed-table updates,
unique checks, rollback, triggers, foreign keys, and index maintenance remain
unsupported.

## Single-File Impact

All row lifecycle state lives in the primary `.mylite` file. No MariaDB durable
sidecars and no MyLite companion files are introduced. Deleted and replaced row
payloads remain orphaned until free-space management and checkpoint compaction
exist.

## File Format Impact

Add a checksummed row-state page:

- magic,
- page type and page version,
- storage format version,
- checksum algorithm,
- page id,
- table id,
- source row id,
- replacement row id,
- state kind,
- checksum.

The storage capability mask gains a row-lifecycle flag. The global file format
version remains unchanged for the current early development line because old
row pages remain readable and the current reader understands the new page type.

## Embedded Lifecycle And API

No public `libmylite` API is added. Existing direct SQL execution should surface
successful keyless `UPDATE`/`DELETE` through MariaDB affected-row behavior and
existing diagnostics.

The handler must release any row-id scan arrays with the same lifetime as scan
row buffers.

## Storage-Engine Routing Impact

Existing engine routing stays in force. Keyless tables routed from omitted
engine, `ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria`
can update/delete rows through full scans. Keyed tables continue to reject
updates/deletes until index and uniqueness slices define correct behavior.

## Test Plan

- Add storage unit tests for:
  - row ids returned with rowsets,
  - deleting one row hides it from scans and row counts,
  - updating one row appends a replacement and hides the old row,
  - close/reopen behavior through normal storage reads,
  - corrupt row-state pages.
- Extend storage-engine smoke coverage:
  - update a keyless non-BLOB row,
  - delete a keyless non-BLOB row,
  - exercise ordered update/delete paths that reposition rows through
    `position()`/`rnd_pos()`,
  - update and delete keyless BLOB/TEXT rows,
  - verify results before and after close/reopen,
  - verify keyed/autoincrement update/delete attempts still fail,
  - assert no forbidden durable sidecars.
- Run the normal dev, embedded, storage-smoke, format, diff, and tidy checks.

## Acceptance Criteria

- Keyless routed table rows have stable row ids in storage rowsets.
- `UPDATE` and `DELETE` work for keyless rows through MariaDB SQL execution.
- Updated/deleted rows stay correct after close/reopen.
- BLOB/TEXT row payload reconstruction still works after update/delete.
- Keyed and autoincrement table update/delete remain explicitly unsupported.
- Compatibility, roadmap, and storage docs describe the partial DML support and
  remaining limits.

## Risks

- The append-only row-state map is O(file pages) per scan. That is acceptable
  for the slice, but row roots and checkpointed state are needed before scale
  claims.
- Failed multi-row statements can leave earlier row-state pages visible because
  there is no transaction layer yet. The compatibility matrix must keep
  rollback planned.
- Updating rows with generated columns, triggers, foreign keys, or indexed
  constraints needs separate designs.
