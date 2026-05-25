# Bounded Range Cursor Continuation

## Problem

Durable forward range cursors now start from a byte-safe lower-bound suffix and
defer row payload reads, but they still materialize every matching key entry in
that suffix before MariaDB consumes the first row. A query such as
`WHERE secondary_key >= ? ORDER BY secondary_key LIMIT 1` should not allocate
and sort the entire remaining published index when a small ordered prefix is
enough.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` positions range access
  through `ha_index_read_map()`. `handler::read_range_next()` advances with
  `ha_index_next()` for non-equality ranges and checks the end bound above the
  storage engine.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` maps
  raw forward `HA_READ_KEY_OR_NEXT` / `HA_READ_AFTER_KEY` keys to
  `MYLITE_INDEX_CURSOR_FORWARD_LOWER_BOUND`.
- `ha_mylite::build_index_cursor()` uses
  `mylite_storage_read_index_entries_from_prefix()` for those durable raw
  lower-bound cursors, then creates an in-memory cursor over the full returned
  entryset.
- `ha_mylite::index_next()` currently returns EOF once the in-memory cursor is
  exhausted; adding a bounded storage read without continuation would be
  incorrect for scans that consume more than one batch.
- `packages/mylite-storage/src/storage.c::read_index_leaf_entries_from_prefix()`
  can stream a suffix from published leaf or branch roots, but it appends live
  tail overlay entries when the root has a tail. Tail overlays can sort before
  later static entries, so a bounded static read is only safe when the root is
  complete and no tail overlay must be merged.

## Scope

- Add a storage read API for a bounded, ordered suffix of static no-tail
  published leaf or branch roots.
- Add handler continuation state for durable raw forward lower-bound cursors.
- Fetch the next bounded batch when `index_next()` reaches the end of the
  current batch.
- Continue inside `index_read_map()` when `HA_READ_AFTER_KEY` skips a bounded
  batch containing only equal keys.
- Keep the existing full entryset path for volatile rows, BLOB/TEXT tables,
  non-raw keys, full scans, exact/prefix match cursors, and any root with a live
  append-tail overlay.

## Non-Goals

- No bounded cursor for append-tail overlay roots in this slice.
- No bounded cursor for BLOB/TEXT row materialization.
- No change to SQL semantics, optimizer estimates, file format, or write paths.
- No multi-range read implementation.

## Design

Storage will expose a bounded lower-bound read that accepts:

- the original lower-bound key prefix,
- an optional exclusive resume `(key, row_id)` pair,
- a maximum entry count,
- output entries plus a completion flag.

For static no-tail leaf and branch roots, storage descends to the first possible
leaf page, lower-bounds inside that page, skips entries at or below the resume
pair, and appends up to the requested limit in physical key order. If the root
is not a complete static published root, storage returns `MYLITE_STORAGE_UNSUPPORTED`
so the handler can fall back to the existing full materialization path.

The handler will use the bounded API only for durable
`MYLITE_INDEX_CURSOR_FORWARD_LOWER_BOUND` cursors. It keeps the current batch in
the existing `index_keys` / `index_entries` buffers, plus enough continuation
metadata to rebuild the next batch after the last returned key. A batch size of
128 entries keeps ordinary LIMIT queries small while still amortizing storage
calls for longer scans.

`index_next_same()` remains unchanged because equality-range cursors still use
the complete match-cursor path.

## Compatibility Impact

Supported SQL results must stay identical. MariaDB still owns range end-bound
checks in `handler::read_range_first()` / `handler::read_range_next()`, so the
handler must never report EOF just because a bounded batch ended while storage
has more static entries.

## Single-File And Lifecycle Impact

No file-format change. Reads stay under the existing scoped read statement and
primary `.mylite` file lifecycle. No new durable companions.

## Public API And File-Format Impact

The storage package gains a first-party C API for bounded index entryset reads.
The `.mylite` file format is unchanged.

## Storage-Routing Impact

Routed `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and explicit MyLite
tables all continue through the MyLite handler. The bounded path is selected by
cursor shape and root shape, not by requested engine name.

## Binary-Size, License, And Dependency Impact

First-party storage and handler code only. No dependency or license change.

## Test Plan

- Add storage unit coverage for bounded static lower-bound reads across a
  published leaf/branch root.
- Add embedded storage-engine coverage showing a forced secondary range
  `LIMIT 1` query reads a bounded number of index entries while still returning
  correct ordered results.
- Add continuation coverage for a range query that consumes more rows than one
  bounded batch.
- Run storage and embedded storage-smoke tests plus formatting and whitespace
  checks.

## Acceptance Criteria

- Short static no-tail secondary range reads allocate only the bounded entry
  prefix needed for the first batch.
- Longer forward range scans continue across batch boundaries without missing
  or duplicating rows.
- Roots with append-tail overlays use the existing full cursor path.
- Docs and roadmap describe the static no-tail continuation limitation.

## Risks And Open Questions

- Append-tail overlays still need a merge cursor before bounded reads can cover
  all durable root shapes.
- The first batch size may need tuning after local benchmark measurements.
