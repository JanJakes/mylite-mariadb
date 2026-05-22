# Inline Insert Append Pages

## Problem

Prepared insert throughput still pays avoidable storage write overhead after
the libmylite prepared-bind cleanup. The row append path writes the inline row
page through one pager write and then writes each fallback append-only
index-entry page through another pager write. The update path already batches
its replacement row, row-state, and changed fallback index-entry pages into one
contiguous append write.

The local baseline before this slice showed:

- `tools/mylite-perf-baseline --phase=prepared-updates 1000 200000`:
  `21.609 us/op` for prepared inserts and `2.309 us/op` for prepared
  primary-key updates.
- `tools/mylite-perf-baseline --phase=storage-pk-row-lookups-one-read 1000
  10000`: `0.522 us/op`, which keeps the current insert overhead clearly
  above raw storage point-read cost.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  writes row payload pages, then maintained-root inserts, then fallback
  append-only index-entry pages before publishing the header.
- `packages/mylite-storage/src/storage.c::write_row_payload_pages()` already
  assigns inline inserted rows to `header->page_count` and reports the first
  following page as `next_page_id`.
- `packages/mylite-storage/src/storage.c::write_index_entry_pages()` writes
  changed fallback index-entry pages consecutively from the supplied first page
  id.
- `packages/mylite-storage/src/storage.c::write_inline_update_pages()` encodes
  multiple adjacent append pages into one buffer and writes them with
  `write_pages_at_raw()`.
- `packages/mylite-storage/src/storage.c::changed_index_entry_count()` and
  `is_index_entry_changed()` already handle a `NULL` change map as all entries
  changed, which matches append rows with no maintained-root plan.

## Design

Add an inline-insert append batching helper beside the existing inline-update
helper. It will:

- apply only to fixed-size storage pages and inline row payloads that do not
  need BLOB overflow pages;
- assign the inserted row page to `header->page_count`;
- encode that row page followed by every changed fallback append-only
  index-entry page into one contiguous buffer;
- write the buffer through `write_pages_at_raw()`;
- report `row_page_id`, first fallback page id, and final `next_page_id` to the
  caller.

The existing overflow-row path remains on `write_row_payload_pages()`. The
existing maintained-root writes remain separate because they dirty existing
root pages protected by the preplanned journal, while the new helper only
coalesces newly appended pages.

## Affected Subsystems

- First-party MyLite storage row append implementation.
- Active statement append buffering, through the already-central
  `write_pages_at_raw()` path.
- Storage-smoke insert performance baseline.

## Compatibility Impact

No SQL-visible behavior changes. Inserted row ids, fallback index-entry page
ids, maintained-root overflow-tail markers, row visibility, duplicate-key
semantics, and handler routing should remain unchanged.

## DDL Metadata Routing Impact

No DDL metadata routing change.

## Single-File And Embedded Lifecycle Impact

All durable bytes still live in the primary `.mylite` file. The change only
coalesces adjacent append writes and still uses the existing rollback journal,
active append buffer, header publication, and checkpoint lifecycle.

## Public API And File-Format Impact

No public API or durable file-format change. Page contents and page ordering
remain the same for inline rows and fallback index-entry pages.

## Storage-Engine Routing Impact

No routing policy change. This affects routed MyLite insert execution only
after a table is already using the MyLite handler.

## Build, Size, And Dependencies

No new dependency. Binary impact is limited to one storage helper and a small
branch in the append path.

## Test Plan

- Add or rely on focused storage unit coverage that appends indexed inline rows
  and verifies row plus index visibility.
- Build and run the MyLite storage unit target.
- Rebuild the storage-smoke MariaDB embedded archive because
  `packages/mylite-storage/src/storage.c` changes.
- Build storage-smoke targets covering the MyLite handler and performance
  baseline.
- Run focused storage-smoke tests for storage-engine behavior.
- Run a local prepared insert/update performance baseline.
- Run `git diff --check`.

## Acceptance Criteria

- Inline row inserts with fallback index-entry pages preserve row reads,
  exact-index reads, maintained-root behavior, rollback, and recovery coverage.
- Overflow row inserts continue through the existing conservative path.
- Existing storage and storage-smoke tests pass.
- Local prepared insert performance improves or stays within noise.

## Verification Results

- Existing indexed append storage tests were reused because they assert exact
  inserted row ids and expected index-entry page offsets for inline rows.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --preset dev --output-on-failure -R mylite-storage`: passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build build libmariadbd.a`: passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --preset storage-smoke-dev --output-on-failure -R
  'mylite-storage|libmylite.embedded-storage-engine'`: passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10 tests.
- `tools/mylite-perf-baseline --phase=prepared-updates 1000 200000`:
  prepared inserts improved from the recorded `21.609 us/op` baseline to
  `6.498 us/op`; prepared primary-key updates were `2.432 us/op`.
- `git diff --check`: passed.
- `git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  no formatting changes.

## Risks And Follow-Up

The helper must pass the same first fallback index page to maintained-root
overflow markers that the old path used. It intentionally does not batch dirty
maintained root pages with the append run; that belongs to a future pager or
WAL slice if maintained-root write coalescing becomes a visible bottleneck.
