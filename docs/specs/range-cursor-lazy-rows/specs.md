# Range Cursor Lazy Rows

## Problem

Forward range cursors can now avoid reading unrelated earlier index leaf pages,
but `ha_mylite::build_index_cursor()` still bulk-materializes every row payload
in the resulting key suffix for non-BLOB tables. Queries such as
`WHERE indexed_int >= ? ORDER BY indexed_int LIMIT 1` should only need the
first qualifying row payload after MariaDB positions the range cursor.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` calls
  `ha_index_read_map()` with the range start key and flag, then validates the
  returned row against `end_range`.
- `mariadb/sql/handler.cc::handler::read_range_next()` continues with
  `ha_index_next()` unless the range is equality-only, in which case it uses
  `ha_index_next_same()`.
- `mariadb/sql/handler.cc::handler::ha_index_read_map()` and
  `ha_index_next()` delegate row production to the storage engine handler.
  LIMIT handling is above this layer, so the handler should avoid reading row
  payloads before MariaDB actually asks for them.
- MariaDB's range planner consults handler `records_in_range()` estimates when
  deciding whether to use range access or fall back to an ordered full-index
  scan.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()`
  builds an ordered key cursor, and for non-BLOB tables calls
  `materialize_index_cursor_rows()` for every cursor entry.
- `ha_mylite::read_index_cursor_row()` already supports lazy row reads when
  `index_rows` is not populated; BLOB tables use that path today.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_indexed_row()`
  reads an indexed row by row id without revalidating full row visibility,
  which is the correct follow-up for a live index entry.

## Scope

- Keep eager key-entry collection and sorting unchanged for this slice.
- Skip eager row-payload materialization for forward lower-bound range cursors.
- Use the existing lazy `read_index_cursor_row()` path when MariaDB requests
  the positioned row or a later row via `index_next()`.
- Provide a conservative MyLite range estimate so routed tables can reach
  MariaDB's range handler path for simple indexed bounds.
- Add test-hook evidence that a forced forward range with `LIMIT 1` reads only
  the row payload it returns.

## Non-Goals

- No incremental B-tree cursor object yet; key suffixes are still eagerly
  collected.
- No LIMIT plumbing from the SQL layer into the handler.
- No change to exact unique lookup, exact/prefix match cursors, reverse range
  starts, full scans, or volatile MEMORY/HEAP cursor behavior.
- No row-payload cache redesign or new public `libmylite` API.

## Design

Treat `MYLITE_INDEX_CURSOR_FORWARD_LOWER_BOUND` as a key-only cursor build.
The cursor still contains sorted key bytes and row ids, so
`mylite_find_index_entry()` and `index_next_same()` keep their current ordering
and positioning behavior. It simply leaves `index_rows`,
`index_row_offsets`, and `index_row_sizes` empty so
`read_index_cursor_row()` performs the row-id lookup only for rows MariaDB
actually requests.

The storage test hook will count uncached durable row-payload page reads. The
embedded storage-engine test will clear storage caches, reset the counter, run a
forced `score >= ... ORDER BY score LIMIT 1` query, and assert one row-payload
read. Without the lazy path, cursor construction reads the full suffix before
LIMIT can stop execution.

MyLite also overrides `records_in_range()` with a coarse estimate. Exact
closed full-key ranges over raw non-nullable keys estimate one row, and other
bounded ranges estimate a small fraction of the table. This is not a selectivity
model; it is enough to keep MariaDB from preferring an ordered full-index scan
for the simple lower-bound shape covered by this slice.

## Compatibility Impact

SQL-visible behavior should not change. MariaDB still owns range planning,
range-end checks, LIMIT handling, virtual-column updates, and key comparison.
The storage engine only changes when row payloads are read.

## Single-File And Lifecycle Impact

No file-format or companion-file changes. Lazy row reads use the same
single-file row-id materialization path already used for BLOB index cursors.

## Public API And File-Format Impact

No public `libmylite` or `.mylite` file-format change. The only new callable
surface is a storage test hook compiled under `MYLITE_STORAGE_TEST_HOOKS`.

## Storage-Routing Impact

The optimization applies to durable MyLite-routed tables, including tables
requested as `ENGINE=InnoDB` and resolved to MyLite. Volatile MEMORY/HEAP keeps
the existing cursor behavior.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size, License, And Dependency Impact

Small first-party handler and test-hook code only. No dependency or license
change.

## Test Plan

- Add storage test-hook counters for uncached row-payload reads.
- Add routed embedded storage-engine coverage for a forced forward integer range
  with `LIMIT 1`, asserting the query returns the expected row and performs one
  row-payload read after caches are cleared.
- Assert the same query's `EXPLAIN` output uses the routed secondary key.
- Run the embedded storage-engine test, storage tests as needed for the new
  hook, formatting checks, and whitespace checks.

## Acceptance Criteria

- Forward lower-bound cursors do not bulk-read row payloads during cursor
  construction.
- `HA_READ_KEY_OR_NEXT` / `HA_READ_AFTER_KEY` results remain correct when the
  first row and later `index_next()` rows are requested lazily.
- Exact/prefix match cursors and full cursors keep their existing behavior.
- The regression test would fail against the previous eager materialization
  path.

## Verification Results

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `cmake --build build/mariadb-mylite-storage-smoke --target mylite_se`
- `git diff --check`
- `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc mariadb/storage/mylite/ha_mylite.h packages/mylite-storage/src/storage.c packages/libmylite/tests/embedded_storage_engine_test.c`

## Risks And Open Questions

- Long range scans may prefer batched row materialization. If this regresses
  whole-range workloads, a later slice should add an adaptive or batched row
  payload path rather than restoring eager reads for short ranges.
- This still leaves eager key suffix collection in place. A true B-tree cursor
  object remains the larger production-navigation step.
