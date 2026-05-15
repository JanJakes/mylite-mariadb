# BLOB/TEXT Row Storage

## Problem

MyLite currently persists MariaDB record images directly. That works for fixed
fields, nullable fields, and ordinary `VARCHAR` data, but it is not durable for
`BLOB` or `TEXT` fields. MariaDB's in-memory record representation stores a
length prefix plus a process pointer to the value bytes; writing that pointer
into the `.mylite` file cannot survive close/reopen and is unsafe even inside a
single process after the source buffers are freed.

Common application schemas use `TEXT`, `LONGTEXT`, `BLOB`, and `LONGBLOB`.
MyLite needs a bounded BLOB/TEXT row path before application-schema work can
produce useful evidence.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/field.h:4458-4668` defines `Field_blob`. Its row slot contains
  `packlength` bytes of length plus `sizeof(char *)` bytes of pointer; the
  durable bytes are not present in the row image.
- `mariadb/sql/field.h:4620-4623` exposes `pack_length_no_ptr()` and
  `row_pack_length()` as the length-prefix portion that belongs in the row
  buffer without the pointer.
- `mariadb/sql/field.h:4648-4668` exposes `get_ptr()` and
  `set_ptr_offset()`, which let a handler read the current value bytes and
  reconstruct a row buffer for another record image.
- `mariadb/sql/field.cc:8898-8984` stores BLOB/TEXT values by allocating or
  reusing `Field_blob::value`, writing the length prefix, and copying a pointer
  into the row slot.
- `mariadb/sql/field.cc:9034-9045` reads BLOB/TEXT values through that pointer;
  a null pointer with zero length is returned as an empty value.
- `mariadb/sql/field.cc:9325-9383` implements `Field_blob::pack()` and
  `Field_blob::unpack()` as length-plus-bytes serialization. That validates
  the approach of storing value bytes separately from the in-memory pointer.
- `mariadb/sql/table.cc:9314-9320` documents that read rows and write rows can
  have distinct `Field_blob` value caches, so a handler must keep reconstructed
  BLOB payload memory alive while MariaDB reads the returned row.

## Design

Add a durable row payload format owned by the MyLite handler for rows that have
one or more BLOB/TEXT fields:

- The handler copies MariaDB's record image.
- For each BLOB/TEXT field, the copied row image keeps the length prefix but
  zeros the process pointer bytes.
- The handler appends descriptors containing field index and payload length,
  followed by the BLOB/TEXT payload bytes.
- Empty values need no payload descriptor because MariaDB treats a zero-length
  null pointer as an empty string; SQL `NULL` continues to be represented by
  MariaDB's null bitmap in the copied row image.
- On scan, the handler reconstructs an in-memory record image, copies durable
  BLOB/TEXT bytes into scan-owned memory, and uses
  `Field_blob::set_ptr_offset()` to point the returned record at those bytes.

Storage remains schema-agnostic. It stores row payload bytes supplied by the
handler and returns variable-size payloads to the handler. Small row payloads
stay inline in checksummed row pages. Larger row payloads use existing
checksummed blob-page chaining with a row-payload page type, while the row page
records the overflow root page and total payload size.

This keeps MariaDB-specific BLOB pointer handling in the MariaDB-facing handler
and keeps `packages/mylite-storage/` focused on page ownership, checksums,
catalog table ids, and row payload lifecycles.

## Non-Goals

- General typed row encoding for every SQL type.
- BLOB/TEXT indexes, prefix indexes, or duplicate checks beyond the existing
  single-column autoincrement key path. A later BLOB/TEXT prefix-index slice
  adds bounded prefix-index support.
- Update/delete, truncate, copy `ALTER`, or free-space reclamation.
- Transaction rollback, crash recovery, or cross-process writer safety.
- Compression or deduplication of BLOB/TEXT payloads.
- External files for large values.

## Compatibility Impact

Keyless MyLite-routed tables with BLOB/TEXT columns can insert, full-scan,
close, and reopen while preserving binary-safe values. The supported
single-column autoincrement key path can also store BLOB/TEXT payloads, but
only the autoincrement key remains checked for duplicates.

Compatibility remains partial because MyLite still lacks index access,
transaction rollback, update/delete, and DDL rebuild support.

## Single-File Impact

All BLOB/TEXT payload bytes live in the primary `.mylite` file. No MariaDB
sidecars and no MyLite companion files are introduced. Large row payloads use
append-only row-payload blob pages until free-space management and transactional
publication exist.

## File Format Impact

The row page layout gains an overflow-root field in previously unused row-page
header bytes. A zero root means the row payload is inline in the row page. A
non-zero root points to a chain of blob pages whose page type is row payload.

The storage capability mask gains a BLOB/TEXT row flag. The global file format
version remains unchanged for the current early development line because the
current reader understands the new page type and old row pages remain valid.

## Embedded Lifecycle And API

No public `libmylite` SQL API is added. The internal storage rowset API gains
variable-size row metadata and a rowset free helper so callers release payload
buffers and offset arrays consistently.

The handler owns reconstructed scan memory until `rnd_end()`, `close()`, or
the next `rnd_init()`. `rnd_next()` copies the row image into MariaDB's output
buffer; any BLOB/TEXT pointers in that copied row continue to point at
scan-owned payload memory.

## Storage-Engine Routing Impact

Existing routing stays in force for omitted/default engine, `ENGINE=MYLITE`,
`ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria`. This slice broadens the
row write predicate from non-BLOB rows to rows whose BLOB/TEXT values can be
serialized into MyLite row payloads.

## Binary Size And Dependencies

No dependency is added. The opt-in MyLite storage smoke profile grows by
first-party serialization, rowset, and overflow-page code. The default embedded
baseline is unchanged because the storage engine profile remains opt-in.

## Test Plan

- Add storage unit coverage for variable-size row payloads.
- Add storage unit coverage for large row payloads that spill into row-payload
  blob pages and survive readback.
- Add corrupt row-payload overflow coverage.
- Extend storage-engine smoke coverage:
  - create a keyless `TEXT`/`BLOB` table routed from `ENGINE=InnoDB`,
  - insert ordinary text, `NULL`, empty string, and binary bytes,
  - select values before and after close/reopen,
  - create an autoincrement table with a `TEXT` payload and verify generated
    values plus BLOB/TEXT reconstruction,
  - assert no forbidden durable sidecars.
- Run the normal dev, embedded, storage-smoke, format, diff, and tidy checks.

## Acceptance Criteria

- BLOB/TEXT payload bytes are never persisted as process pointers.
- Keyless routed BLOB/TEXT rows insert and full-scan correctly before and after
  close/reopen.
- Large row payloads can exceed a single row page by using primary-file
  overflow pages.
- SQL `NULL`, empty BLOB/TEXT values, ordinary text, and binary bytes are
  covered by tests.
- Existing non-BLOB row storage and autoincrement behavior continue to pass.
- Compatibility, roadmap, and storage architecture docs describe the new
  partial support without claiming indexes, update/delete, or transactions.

## Implementation Status

Implemented in the storage package and MyLite handler:

- `mylite_storage_rowset` now carries variable-size row offsets and sizes, with
  `mylite_storage_free_rowset()` for cleanup.
- Row pages can point at row-payload blob-page chains when a stored row payload
  exceeds the inline row-page capacity.
- `ha_mylite::write_row()` serializes BLOB/TEXT fields into durable row payload
  bytes before appending them to storage.
- `ha_mylite::rnd_init()` reconstructs MariaDB row buffers and keeps scan-owned
  BLOB/TEXT payload memory alive until the scan ends.
- Storage unit tests cover variable row payloads, large overflow row payloads,
  and corrupt overflow pages.
- Storage-engine smoke tests cover `TEXT`, `BLOB`, `NULL`, empty values, binary
  bytes, large overflow text, close/reopen, and the single-column
  autoincrement key path with a `TEXT` payload.

## Risks

- The row serializer is still coupled to MariaDB's table-definition image.
  That is acceptable for this stage, but later format work needs explicit
  upgrade and migration rules.
- Scan reconstruction keeps BLOB/TEXT payloads in scan-owned memory. That is
  correct for current full scans, but positional reads and update/delete will
  need row identifiers and a tighter lifetime model.
- Publication remains append-only and non-transactional until the recovery
  slice defines atomic page visibility.
