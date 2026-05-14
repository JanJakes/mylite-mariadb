# Row Storage Foundation

## Goal

Add the first durable MyLite row-storage path: `INSERT` appends rows for
keyless MyLite-routed tables into the primary `.mylite` file, and full table
scans read those rows back through MariaDB's handler interface after close and
reopen.

## Non-Goals

- Do not implement primary, unique, or secondary indexes.
- Do not allow writes into keyed tables before duplicate-key and ordered-index
  behavior exists.
- Do not implement update, delete, truncate, copy `ALTER`, or catalog-changing
  DDL.
- Do not implement autoincrement, BLOB/TEXT overflow pages, generated-column
  storage, FULLTEXT, SPATIAL, or foreign-key enforcement.
- Do not claim transaction rollback, crash recovery, or concurrent writer
  safety beyond the current single-process smoke behavior.
- Do not add MariaDB durable sidecars as a compatibility fallback.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/handler.h:4359-4410` requires handlers to implement
  `rnd_next()`, `rnd_pos()`, and `position()` for table reads.
- `mariadb/sql/handler.h:5232-5244` documents that `rnd_init()` may be called
  repeatedly for scans and that `write_row()` defaults to
  `HA_ERR_WRONG_COMMAND` until a handler overrides it.
- `mariadb/sql/handler.h:5254-5258` leaves `update_row()` unsupported by
  default, which is the right behavior for this slice.
- `mariadb/storage/example/ha_example.cc:365-381` describes the `write_row()`
  input as MariaDB's native row byte array and points engines to `TABLE` field
  metadata when they need value extraction.
- `mariadb/storage/heap/ha_heap.cc:288-307` shows a handler can write the
  incoming record buffer directly, while `ha_heap.cc:398-406` shows
  `rnd_init()` and `rnd_next()` as the scan boundary.

MariaDB already owns SQL expression evaluation, type conversion, defaults, and
the row image passed to the handler. This slice can therefore persist MariaDB
record images without inventing an early MyLite SQL type encoder. That choice
is deliberately temporary: indexes, overflow values, online format upgrades,
and logical migration will need a richer native row format later.

## Compatibility Impact

Compatibility moves from metadata-only tables to partial row behavior.
Applications can insert into and scan keyless tables routed from omitted engine,
`ENGINE=MYLITE`, `ENGINE=InnoDB`, `ENGINE=MyISAM`, or `ENGINE=Aria` requests.
Tables with any declared key continue to reject row writes until index and
duplicate-key behavior is implemented, because accepting those writes would
violate MySQL/MariaDB uniqueness expectations.

`docs/COMPATIBILITY.md` must mark fixed and variable row fields as partial for
keyless table scans only. Indexes, uniqueness, autoincrement, BLOB/TEXT
overflow, update/delete, transactions, and recovery remain planned.

## Design

The MyLite handler remains thin:

- `ha_mylite::write_row()` rejects keyed tables and autoincrement tables, then
  appends the `TABLE_SHARE::reclength` bytes from MariaDB's record buffer to
  MyLite storage.
- `ha_mylite::rnd_init()` loads a read cursor for the current table.
- `ha_mylite::rnd_next()` copies the next stored record image into MariaDB's
  output buffer and returns `HA_ERR_END_OF_FILE` when exhausted.
- `ha_mylite::info()` reports exact row count for the table where practical.
- `update_row()` and `delete_row()` remain unsupported through the base handler
  implementation.

The first storage format uses append-only row pages that are independent from
the catalog record. Each row page stores:

- row page magic and version fields,
- page id and checksum,
- catalog table id,
- row record size,
- row count,
- raw MariaDB record bytes.

The catalog record already has a table id. Row appends find that id from the
catalog, append a row page at the current file end, and update the header page
count. Full scans read all pages after the catalog and definition pages, select
valid row pages for the table id, validate checksums and row sizes, and return
a contiguous rowset to the handler.

This intentionally favors a reviewable first path over locality. Later slices
should add table row roots, free-space management, row ids, indexes, and
transactional page publication.

## DDL Metadata Routing Impact

No new DDL is added. Existing routed table-definition metadata remains the
authority for table discovery. The row-storage API relies on catalog table ids
derived from those metadata records.

## Storage-Engine Routing Impact

The engine-routing policy from the previous slice stays in force. This slice
adds row behavior for the accepted routed engines only when the table has no
keys and no autoincrement column. Unsupported explicit engines still fail
before catalog publication.

## File Lifecycle

Rows are durable state inside the primary `.mylite` file. This slice must not
create persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`,
redo, undo, binlog, relay-log, or plugin-owned table files. The existing
storage smoke sidecar scanner must cover row DML and close/reopen.

This slice does not add journals, WAL, locks, or recovery companions.
Publication remains non-transactional and is documented as a risk until the
transactions and recovery slice.

## Embedded Lifecycle And API

No public `libmylite` API changes are required. `mylite_exec()` should surface
successful keyless `INSERT` and `SELECT` behavior through the existing direct
execution callback. Error diagnostics for unsupported keyed writes continue to
come from MariaDB handler errors.

`rnd_init()` must be safe to call repeatedly on the same handler, replacing any
existing scan cursor before loading a fresh one. `close()` must release any
storage-owned row buffers.

## Build, Size, And Dependencies

No new dependency is introduced. The storage-smoke static MyLite handler build
will grow by first-party row-page and scan code only. The default embedded
baseline remains unchanged because the MyLite storage engine smoke profile is
opt-in.

## Implementation Status

Implemented for keyless MyLite-routed tables. The current row-page encoder
stores one MariaDB record image per checksummed row page, tagged with the
catalog table id. Full scans read every row page in the file and copy matching
record images into MariaDB's output buffer. Writes into keyed or autoincrement
tables still return unsupported handler errors.

## Test Plan

1. Add storage unit coverage for appending raw record bytes and reading them
   back for one table.
2. Add storage unit coverage proving rows for two catalog tables stay separated
   by table id.
3. Add corrupt row-page coverage for checksum validation.
4. Extend storage-engine smoke coverage:
   - create a keyless routed table,
   - insert multiple rows,
   - select them back through `mylite_exec()`,
   - close and reopen,
   - select the same rows again,
   - assert known durable sidecars are absent.
5. Keep a keyed routed table insert test that must still fail before index
   support exists.
6. Run `storage-smoke-dev`, `dev`, `embedded-dev`, format checks, clang-tidy,
   and `git diff --check`.

## Acceptance Criteria

- Keyless routed tables support durable `INSERT` plus full table scans.
- Inserted rows survive close and reopen.
- Keyed tables still reject row writes until index support exists.
- Storage unit tests cover row append, row scan, and table-id separation.
- Row pages are checksummed and validated before scan results are returned.
- Row DML leaves no known MariaDB durable sidecars.
- Compatibility, roadmap, and storage architecture docs describe the partial
  row-storage foundation without claiming indexes or transactions.

## Risks And Open Questions

- Raw MariaDB record images couple stored rows to MariaDB's table-definition
  image. That is acceptable for the first scan path, but later format work must
  define upgrade and migration rules.
- Scanning all pages to find a table's row pages is inefficient. It avoids a
  catalog mutation path before transactions exist, but row roots and page
  chains should replace it.
- There is no rollback or crash recovery yet. A crash during row append can
  leave the primary file with a stale header or trailing row page. The recovery
  slice must define atomic publication before broader write claims.
- The first write path is deliberately single-process smoke behavior. File
  locks and concurrent writer semantics remain planned.
