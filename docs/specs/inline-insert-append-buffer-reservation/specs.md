# Inline Insert Append Buffer Reservation

## Problem Statement

The prepared insert component benchmark shows that bind and reset costs are
small, while the `step` segment still costs about `5 us/op` locally for a
routed `ENGINE=InnoDB` table using MyLite storage. The current inline insert
storage helper encodes the row page and fallback index-entry pages into a stack
or heap buffer, then `write_pages_at_raw()` copies that run into the active
append buffer during explicit transactions.

That extra memory copy is avoidable for the common inline-row insert shape.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` builds MyLite
  row payload and index-entry records for MariaDB insert execution, then calls
  `mylite_storage_append_row_with_index_entries()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  plans maintained-root inserts, begins the journal, writes inline append pages
  for small rows, then publishes the updated header.
- `packages/mylite-storage/src/storage.c::write_inline_insert_pages()` encodes
  the row and changed fallback index-entry pages into a temporary contiguous
  buffer before calling `write_pages_at_raw()`.
- `packages/mylite-storage/src/storage.c::buffer_append_pages_at_raw()` is the
  active checkpoint append-buffer entry point used by `write_pages_at_raw()`;
  it verifies the write is a full-format contiguous append at the current
  active page count, ensures bounded capacity, copies the supplied pages, and
  makes them visible to reads before commit.
- `packages/mylite-storage/src/storage.c::read_page_at()` already reads active
  append-buffer pages before falling back to the primary file.

## Scope

- Add a lower-level append-buffer reservation helper for full-format
  contiguous append runs.
- Use it from inline inserts so small row plus fallback index-entry pages can
  be encoded directly into the active append buffer when the same conditions as
  `buffer_append_pages_at_raw()` are satisfied.
- Keep the existing temporary-buffer and direct-write path when no active
  append buffer is eligible.
- Preserve existing active append-buffer semantics for other callers of
  `write_pages_at_raw()`.

## Non-Goals

- No durable file-format change.
- No maintained-root dirty-page coalescing.
- No BLOB/TEXT overflow-row path change.
- No public API, SQL, or storage-engine routing behavior change.
- No claim of SQLite-like write throughput; this is one hot-path copy removal.

## Design

Introduce `reserve_append_pages_at_raw()` next to `buffer_append_pages_at_raw()`.
The helper mirrors the existing append-buffer eligibility checks:

- only `MYLITE_STORAGE_FORMAT_PAGE_SIZE` page writes;
- an active checkpoint for the target file and no active read snapshot;
- a first page id equal to the active checkpoint's current page count;
- a contiguous continuation of any existing buffered run;
- bounded total buffered pages.

When eligible, it extends the append buffer and returns a writable page range.
`buffer_append_pages_at_raw()` keeps using the same helper and copies the
caller-provided pages into the reserved range, preserving existing behavior.
`write_inline_insert_pages()` uses the helper first and encodes row and
index-entry pages directly into the returned range. If reservation is not
available, it uses the current stack/heap buffer and `write_pages_at_raw()`
fallback unchanged.

## Affected Subsystems

- First-party MyLite storage append-buffer internals.
- Inline insert row/index append path.
- Storage-smoke prepared insert performance baseline.

## Compatibility Impact

No SQL-visible behavior changes. Inserted row ids, index-entry page ordering,
transaction visibility, rollback behavior, and reads from uncommitted active
checkpoints remain governed by the existing append-buffer and checkpoint
lifecycle.

## DDL Metadata Routing Impact

No DDL metadata routing change.

## Single-File And Embedded Lifecycle Impact

Durable state remains in the primary `.mylite` file. The reservation writes to
the existing process-local append buffer and still flushes before durable
header publication or rollback truncation according to the existing checkpoint
rules.

## Public API, File Format, And Routing Impact

No public C API, file-format, or storage-engine routing impact.

## Build, Size, And Dependencies

No dependency or license change. Binary impact is limited to first-party C
storage code.

## Test Plan

- Add focused storage coverage for indexed inserts inside an active transaction
  and savepoint rollback.
- Build and run `mylite_storage_test`.
- Build and run the storage-smoke embedded storage-engine test.
- Run focused storage-smoke `ctest` coverage for storage and embedded storage.
- Run `prepared-insert-components` before and after the change.
- Run `git diff --check`.
- Run `git clang-format --diff` on changed C files.

## Acceptance Criteria

- Inline inserted rows and index entries remain visible inside active
  transactions before commit.
- Savepoint rollback hides rows and index entries appended through the
  reservation path while preserving earlier inserts.
- Reopen sees committed rows and not rolled-back rows.
- The local prepared insert `step` metric improves or stays within noise.
- Existing focused storage and embedded storage tests pass.

## Verification Results

Local environment: macOS worktree, `storage-smoke-dev` preset.

- Before this slice, after rebuilding the reverted baseline:
  `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` reported
  `prepared-insert-step` at `4.975 us/op`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|embedded-storage-engine' --output-on-failure`: passed.
- Three post-change runs of
  `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` reported
  `prepared-insert-step` at `4.871`, `4.870`, and `4.844 us/op`.
- `git diff --check`: passed.

## Risks And Open Questions

- The improvement may be modest because checksum calculation, row/index page
  encoding, handler work, and MariaDB execution still remain in the step path.
- The append buffer is still a bounded process-local buffer, not a full pager
  or WAL design.
