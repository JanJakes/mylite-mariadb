# MEMORY Engine Routing

## Problem

MyLite already routes durable common engine requests to the MyLite handler and
supports `ENGINE=BLACKHOLE` as a zero-file table whose rows are discarded. The
remaining planned zero-file surface in the engine-routing roadmap is
`ENGINE=MEMORY` / `ENGINE=HEAP`.

Routing MEMORY tables to ordinary durable MyLite row pages would be simple, but
it would be semantically wrong. MariaDB persists MEMORY table definitions while
storing row contents in RAM; after a server restart the table definition
remains and the table is empty. MyLite needs the same product shape: durable
metadata in the primary `.mylite` file and volatile rows tied to the embedded
runtime, with no MariaDB MEMORY data files or native HEAP handler ownership.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:133-139` aliases `HEAP` to `MEMORY` during storage
  engine resolution.
- `mariadb/storage/heap/ha_heap.cc:87-134` opens a named HEAP share if it
  exists, otherwise creates an in-memory share for the table definition.
- `mariadb/storage/heap/ha_heap.cc:64-75` registers the native HEAP handler as
  `DB_TYPE_HEAP` without durable row files.
- The MariaDB MEMORY documentation states that MEMORY contents live in memory,
  table data is lost on server restart, definitions are recreated on restart,
  and the recreated tables are empty:
  <https://mariadb.com/docs/server/server-usage/storage-engines/memory-storage-engine>.
- The same documentation states that `BLOB` and `TEXT` columns are unsupported
  for native MEMORY tables and that hash indexes are the default while B-tree
  indexes are allowed.

## Scope

- Accept explicit `ENGINE=MEMORY` and `ENGINE=HEAP` table creation through the
  MyLite handler.
- Store requested engine metadata as the application-requested MEMORY/HEAP
  token with effective engine `MYLITE`.
- Keep table definitions discoverable after final close/reopen through the
  primary `.mylite` catalog.
- Keep row contents in a process-runtime volatile row store owned by the
  MyLite handler, not in the durable `.mylite` row/index pages.
- Clear volatile MEMORY/HEAP rows when the embedded MariaDB runtime shuts down.
- Support representative insert, full scan, forced-index lookup, update,
  delete, truncate, duplicate-key, and autoincrement behavior during the
  current runtime.
- Keep no-durable-sidecar gates clean.

## Non-Goals

- Native HEAP/MEMORY handler integration.
- Durable MEMORY rows in the primary `.mylite` file.
- Full native MEMORY hash-index parity. MyLite keeps the current supported
  B-tree-compatible key policy for routed tables.
- `max_heap_table_size`, `MAX_ROWS`, and native MEMORY memory-accounting
  enforcement.
- MEMORY replication/binlog behavior.
- Multi-statement transaction or savepoint semantics for volatile rows.

## Design

Extend the MyLite handler's requested-engine whitelist with MEMORY and HEAP.
The catalog stores the canonical MariaDB table definition plus requested and
effective engine names exactly as other routed tables do.

When a handler instance opens a table whose requested engine is MEMORY or HEAP,
it marks the instance as volatile. Volatile handler operations use a
process-global, mutex-protected row store keyed by primary file, schema, and
table name:

1. `create()` stores durable table metadata and creates or clears the volatile
   table entry.
2. `write_row()` serializes the MariaDB record image with the same BLOB/TEXT
   payload format used by durable MyLite rows, then appends it only to the
   volatile store.
3. Full scans and supported index reads build handler cursors from volatile
   rows.
4. `update_row()` and `delete_row()` hide the current volatile row and publish
   the replacement or deletion in the volatile store.
5. `truncate()` clears volatile rows and resets volatile autoincrement state.
6. `delete_table()` and `rename_table()` drop or rename matching volatile rows.
7. `mylite_done_func()` clears all volatile tables at embedded runtime
   shutdown, matching MariaDB's server-restart empty-table behavior.

The existing durable storage statement checkpoint remains responsible for
catalog and durable row rollback. MEMORY/HEAP rows are non-durable,
non-transactional runtime state for this slice.

## Compatibility Impact

`ENGINE=MEMORY` and `ENGINE=HEAP` move from unsupported explicit engine
requests to partial routed support. Metadata survives close/reopen in the
primary `.mylite` file; row contents are visible only within the current
embedded runtime and are empty after final runtime shutdown and reopen.

MyLite does not claim native MEMORY parity for hash indexes, memory limits,
replication, or all native unsupported column/index combinations. Unsupported
table shapes still fail before catalog publication through the existing MyLite
table-shape gates.

## Binary Size Impact

Measured on 2026-05-15 after the storage-smoke rebuild:

- `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` grows from
  32,160,464 bytes / 30.67 MiB to 32,198,224 bytes / 30.71 MiB.
- Archive members increase from 694 to 695 because the volatile row store is a
  separate MyLite storage-engine source file.
- The linked storage-engine smoke test grows from 19,912,256 bytes / 18.99 MiB
  to 19,933,776 bytes / 19.01 MiB before stripping, and from 17,940,496 bytes /
  17.11 MiB to 17,957,088 bytes / 17.13 MiB after stripping.

## File Lifecycle

No MariaDB `.frm`, HEAP data, or plugin-owned durable files are created.
Durable MyLite state is limited to MEMORY/HEAP table metadata in the primary
`.mylite` file. Volatile rows live in process memory and are cleared when the
embedded runtime ends.

## Public API And File-Format Impact

No public `libmylite` API changes. No file-format version change is required:
the existing table-definition record already stores requested and effective
engine names, and volatile row contents are deliberately absent from durable
storage.

## Storage-Engine Routing Impact

The engine-routing policy expands the zero-file supported set from BLACKHOLE to
BLACKHOLE plus MEMORY/HEAP. Durable common-engine routing remains unchanged.

## Test And Verification Plan

- Add storage-engine smoke coverage for explicit `ENGINE=MEMORY`.
- Add storage-engine smoke coverage for explicit `ENGINE=HEAP` alias routing.
- Verify requested/effective catalog metadata and `SHOW CREATE TABLE` engine
  reporting.
- Verify insert/select, forced-index lookup, duplicate-key rejection,
  update/delete, truncate, and autoincrement reset during the current runtime.
- Verify MEMORY/HEAP row contents are empty after final close/reopen while
  table metadata survives.
- Verify no durable sidecars after close/reopen.
- Run the focused storage-engine compatibility group, full storage-smoke tests,
  formatting, shell syntax, whitespace, and tidy checks.

## Acceptance Criteria

- `ENGINE=MEMORY` and `ENGINE=HEAP` create MyLite-routed catalog records.
- MEMORY/HEAP rows are queryable only during the current embedded runtime and
  never appear in durable MyLite row storage.
- MEMORY/HEAP metadata and empty-table reopen behavior survive final
  close/reopen.
- Supported indexes and autoincrement work for representative volatile rows.
- Roadmap, compatibility, and architecture docs distinguish MEMORY/HEAP
  volatile support from durable MyLite engine routing and BLACKHOLE row
  discard.

## Risks And Open Questions

- Native MEMORY defaults to hash indexes. MyLite initially supports only the
  current B-tree-compatible key surface; unsupported hash-only behavior must
  stay explicit.
- This slice treats MEMORY/HEAP rows as non-transactional runtime state. Broader
  transaction/savepoint behavior should be revisited when MyLite implements full
  SQL transactions.
- The process-global volatile store is appropriate for the current single-file
  embedded runtime, which permits one primary file per running MariaDB runtime.
  If future runtime work allows multiple primary files simultaneously, the key
  already includes the primary filename but concurrency coverage must expand.
