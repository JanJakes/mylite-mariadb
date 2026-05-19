# BLOB Index Cursor Lazy Rows

## Problem

The routed storage smoke can mark `wp_posts` as crashed on a forced secondary
index lookup over a WordPress-shaped table:

```sql
SELECT post_title
FROM wp_posts FORCE INDEX (post_name)
WHERE post_name = 'hello-world'
```

`wp_posts` contains BLOB/TEXT columns. The handler currently batch-materializes
indexed row payloads for secondary cursors, then decodes BLOB payload slots from
that batch. The fixed-record fast path already avoids inline durable rows for
BLOB/TEXT tables; the general index cursor path needs the same lifetime
boundary until BLOB/TEXT batch materialization is hardened.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` constructs
  cursor entries, then calls `materialize_index_cursor_rows()` for durable
  cursor rows.
- `mariadb/storage/mylite/ha_mylite.cc::read_index_cursor_row()` has a lazy
  path that reads the selected row by row id when `index_rows` is not set.
- `mariadb/storage/mylite/ha_mylite.cc::preserve_record_blob_payloads()` shows
  that BLOB fields need explicit per-record payload ownership because joined
  result evaluation can outlive scan or cursor buffers.
- The WordPress fixture smoke exercises a non-unique secondary index on
  `wp_posts(post_name)` with BLOB/TEXT columns.

## Design

- Keep batch row materialization for fixed-record indexed cursors.
- For tables with BLOB/TEXT fields, keep index cursor entries but leave row
  payload materialization lazy so `read_index_cursor_row()` reads and owns one
  selected row at a time.
- Apply the same guard to exact unique cursor construction when the row was not
  inlined through the fixed-record storage fast path.
- Do not change index filtering, ordering, row-id selection, or BLOB/TEXT SQL
  compatibility.

## Compatibility Impact

This is a correctness guard. It preserves supported SQL behavior for
BLOB/TEXT-bearing application tables and avoids reporting table corruption on
valid indexed reads.

## Single-File And Lifecycle Impact

No file-format or companion-file change. Durable BLOB/TEXT row payloads remain
owned through the existing lazy row-id read path.

## Storage-Engine Routing Impact

Routed durable MyLite tables with BLOB/TEXT columns no longer use the
batch-materialized indexed-row payload path. Fixed-record tables keep the
performance path.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is a small handler branch.

## Test And Verification Plan

- Run the storage-engine smoke, including WordPress fixture indexed reads.
- Run the storage unit test.
- Run the local performance baseline to confirm fixed-record index loops still
  use the fast path.
- Run `git diff --check`.

## Acceptance Criteria

- The WordPress `wp_posts FORCE INDEX(post_name)` smoke stays green.
- Fixed-record index cursor performance is not intentionally regressed.
- Existing BLOB/TEXT row ownership behavior remains unchanged.

## Risks

- BLOB/TEXT secondary cursor reads remain slower than fixed-record cursor reads
  until the batch materialization path has explicit BLOB payload lifetime
  handling.
