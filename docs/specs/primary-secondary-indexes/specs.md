# Primary And Secondary Indexes

## Problem

MyLite currently persists routed table definitions and row payloads, but general
keyed tables are not writable. The handler only supports a narrow
single-column `AUTO_INCREMENT` duplicate check, `index_flags()` advertises no
ordered access capability, and keyed update/delete remains rejected. This keeps
ordinary application schemas from using primary keys, unique constraints,
secondary lookup keys, or MariaDB optimizer index paths.

This slice adds the first real MyLite index layer for ordinary MariaDB BTREE
keys while preserving the product invariant that durable state lives in the
primary `.mylite` file.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h` defines the storage-engine index contract through
  `index_init()`, `index_end()`, `index_read_map()`,
  `index_read_idx_map()`, `index_next()`, `index_prev()`,
  `index_first()`, `index_last()`, and `index_next_same()`. The public
  `ha_*` wrappers in `mariadb/sql/handler.cc` set `active_index`, update row
  statistics, and require engines to return MariaDB row buffers.
- `mariadb/include/my_base.h` defines `ha_rkey_function` values used by the SQL
  layer for exact, next, previous, prefix, and range positioning.
- `mariadb/sql/key.cc:key_copy()` creates MariaDB key-tuple bytes from a record
  buffer, including null-byte prefixes and VARCHAR/BLOB key images.
- `mariadb/sql/key.cc:key_tuple_cmp()` compares two key tuples according to
  the field types, collations, null ordering, and reverse-sort flags already
  present in `KEY_PART_INFO`.
- `mariadb/sql/key.cc:key_buf_cmp()` checks equality between key buffers and is
  useful for duplicate checks.
- `mariadb/sql/handler.cc:ha_write_row()` calls
  `ha_check_inserver_constraints()` before `write_row()` unless the engine
  advertises `HA_CHECK_UNIQUE_AFTER_WRITE`. Ordinary unique-key errors still
  need the engine to identify the duplicate key through `info(HA_STATUS_ERRKEY)`
  or `lookup_errkey`.
- `mariadb/sql/sql_insert.cc:Write_record::locate_dup_record()` can recover the
  conflicting row by `ha_index_read_idx_map()` when the engine does not
  advertise `HA_DUPLICATE_POS`.
- `mariadb/storage/myisam/ha_myisam.cc` and
  `mariadb/storage/heap/ha_heap.cc` show the expected pattern: advertise
  `HA_READ_NEXT`, `HA_READ_PREV`, `HA_READ_ORDER`, and `HA_READ_RANGE` for
  ordered indexes, initialize an active index cursor, and implement reads and
  directional navigation through the handler methods.
- `mariadb/storage/mylite/ha_mylite.cc` already stores row page ids in
  `position()` / `rnd_pos()`, which gives a stable handler row reference for
  update/delete and duplicate-row lookup.

## Design

Add append-only index-entry pages to `packages/mylite-storage`. Each index
entry stores:

- catalog table id,
- MariaDB key number,
- row page id,
- key-tuple bytes produced by `key_copy()`.

The first implementation does not introduce a B-tree. Reads build an ordered
in-memory cursor from live append-only index entries, sorted with MariaDB's
`key_tuple_cmp()` and row-id tie breaking. This is intentionally an early
correctness-first index representation:

1. `write_row()` validates duplicate unique keys, serializes the row payload,
   extracts one key tuple for every supported table key, and asks storage to
   append the row plus its index entries before publishing the new header page
   count.
2. `update_row()` validates unique keys while ignoring the current row id,
   appends a replacement row, appends a row-state replacement page for the old
   row, and appends new index entries for the replacement row before publishing
   the header.
3. `delete_row()` keeps the existing row-state delete path. Index entries for
   deleted rows remain on disk but are ignored because storage filters entries
   through the live row-state map.
4. `index_init()` and `index_read_idx_map()` read live index entries, read their
   row payloads, reconstruct MariaDB row buffers, sort by key, and position the
   cursor for exact, prefix, next, previous, first, and last reads.

The append-only index-entry format is durable and belongs to the primary
`.mylite` file. A later storage-structure slice can replace the cursor build
with B-tree or pager-backed navigation without changing SQL-visible behavior.

## Supported Scope

- Ordinary MariaDB BTREE-like keys represented by `HA_KEY_ALG_UNDEF` or
  `HA_KEY_ALG_BTREE`.
- Primary keys, unique keys, and non-unique secondary keys over non-BLOB key
  parts whose full MariaDB key tuple fits in one index-entry page.
- Nullable unique-key semantics: if a unique key contains a nullable part and
  the key is not marked `HA_NULL_ARE_EQUAL`, rows with NULL in that key do not
  conflict.
- Ordered reads through `HA_READ_KEY_EXACT`, `HA_READ_KEY_OR_NEXT`,
  `HA_READ_KEY_OR_PREV`, `HA_READ_AFTER_KEY`, `HA_READ_BEFORE_KEY`,
  `HA_READ_PREFIX`, `HA_READ_PREFIX_LAST`, and
  `HA_READ_PREFIX_LAST_OR_PREV`.
- Insert, update, delete, close/reopen, and copy `ALTER` rebuilds for supported
  keyed table shapes.
- Omitted/default engine requests and explicit `ENGINE=MYLITE`,
  `ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` requests that route to
  MyLite.

## Non-Goals

- B-tree page splits, free-space management, index compaction, or cost-quality
  optimization.
- Transaction rollback, statement rollback, savepoints, crash recovery, or
  concurrency guarantees for index updates.
- FULLTEXT, SPATIAL, vector, generated, expression, hash, long-hash,
  application-time-period, foreign-key, or BLOB/TEXT prefix indexes.
- Key-only reads, index condition pushdown, multi-range read optimization, or
  invisible-index policy beyond MariaDB metadata preservation.
- Clustered-index physical row layout.

## Compatibility Impact

Primary keys, unique keys, and secondary indexes move from planned to partial
for supported key shapes. MyLite should still document index support as partial
because the first implementation prioritizes correctness and durability over
large-table performance, transaction rollback, and crash recovery.

`ENGINE=InnoDB`, `ENGINE=MyISAM`, and `ENGINE=Aria` table definitions continue
to resolve to the effective MyLite engine. The requested engine remains
catalog metadata; supported index behavior is provided by MyLite storage, not
by durable InnoDB/MyISAM/Aria sidecars.

## DDL Metadata Routing Impact

`CREATE TABLE` and copy `ALTER` should accept supported keyed table shapes and
reject unsupported index definitions before publishing catalog metadata.
`RENAME TABLE` keeps table ids, so existing row and index-entry pages remain
associated with the renamed table. `DROP TABLE` removes the live catalog record;
old row and index-entry pages become unreachable until free-space reclamation
exists.

## Single-File And File-Format Impact

Add a checksummed `MYLIDX1` index-entry page type to the primary `.mylite`
file. No external `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, or Aria log files are
introduced. Index-entry pages are append-only and filtered through existing
row-state pages for live-row visibility.

The storage capability flags gain an index-entry capability. Existing format
version remains `1` because this repository has not shipped a stable external
file format; compatibility gating still rejects corrupt or newer headers.

## Embedded Lifecycle And API

No public `libmylite` SQL API is added. The internal `mylite-storage` C API
gains index-entry structs and row-with-index write functions used by the
MariaDB handler. Returned storage allocations continue to be freed through
`mylite_storage_free()` or a matching free helper.

## Binary-Size And Dependency Impact

No new third-party dependency is added. The default embedded archive should not
grow outside normal first-party code size; the storage-smoke MariaDB archive
size should be recorded during verification.

## Test Plan

- Add first-party storage tests for:
  - index-entry capability reporting,
  - appending row plus multiple index entries in one published write,
  - reading index entries after close/reopen,
  - filtering old index entries after update/delete row-state pages,
  - rejecting corrupt index-entry pages.
- Extend embedded storage-engine smoke tests for:
  - primary-key insert, duplicate-key failure, ordered lookup, and reopen,
  - unique secondary keys with duplicate failure and multiple NULL values,
  - non-unique secondary-key range/order reads,
  - keyed update/delete through both primary and secondary predicates,
  - `ENGINE=InnoDB` and other supported requested engines resolving to MyLite,
  - copy `ALTER ... ADD INDEX` or `ADD PRIMARY KEY` over supported shapes,
  - no forbidden durable sidecars.
- Run the normal dev, embedded, storage-smoke, tidy, format, diff, and
  storage-smoke MariaDB archive size checks.

## Acceptance Criteria

- Supported keyed tables can insert, scan, update, delete, and reopen through
  the MyLite handler.
- Unique-key violations return duplicate-key errors with the correct key
  number.
- Index reads satisfy MariaDB exact, prefix, ordered next/previous, first, and
  last handler calls for supported keys.
- Supported copy ALTER keyed rebuilds leave one live catalog table and no
  temporary catalog leaks.
- Storage tests prove index-entry pages are durable, checksummed, and filtered
  through row-state visibility.
- Roadmap, compatibility, and storage architecture docs describe the partial
  index support and remaining limits.

## Risks

- The append-only format can accumulate stale index entries after update/delete.
  Free-space management and compaction must address this later.
- Without transaction/recovery support, a process crash during a multi-page
  write can still leave unpublished trailing pages or a torn header update.
  The transaction and recovery slice remains required before strong durability
  claims.
- Cursor builds are O(n) over index entries and rows. This is acceptable for
  correctness coverage now but must not be presented as the long-term
  performance architecture.
