# Active Packed Row Append

## Problem

MyLite can read fixed-size packed row pages and can represent slot row ids with
marked 64-bit references, but ordinary insert execution still writes one
4096-byte row page per small row. That keeps write amplification high for the
hot prepared insert path and delays real storage-size wins from the packed row
format.

The first production writer should be bounded to the active append-buffer path
so rollback and read-your-writes behavior stay inside existing checkpoint
machinery.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc:3929-4086` routes durable
  `ha_mylite::write_row()` calls to
  `mylite_storage_append_row_with_index_entries()` and stores the returned
  64-bit row id in `current_row_id`.
- `mariadb/storage/mylite/ha_mylite.cc:3628-3699` reads rows by that opaque
  64-bit id in `rnd_pos()` and returns the same id through `position()`.
- `mariadb/sql/handler.cc:7547` and `8172` call `position()` before row-id
  filtering or update self-comparison, so the handler ref buffer must keep
  carrying a stable eight-byte value.
- `packages/mylite-storage/src/storage.c::write_inline_insert_pages()` already
  batches one inline row page plus changed append-only index-entry pages into
  the active append buffer when a statement or transaction checkpoint is open.
- `packages/mylite-storage/src/storage.c::reserve_append_pages_at_raw_for_statement()`
  only buffers contiguous appends at the active checkpoint page count.
- Buffered page rollback already has prefix/range undo helpers used by active
  update rewrites; appending a slot to an existing buffered packed row page
  must capture an undo when the page predates the current nested savepoint.

## Design

- Add a small active-statement cache for the most recent buffered packed row
  page:
  - physical row page id;
  - table id;
  - row size;
  - current slot count.
- For no-index inline fixed-size inserts while an active append buffer exists:
  - append to the cached packed page when it belongs to the same table, has
    the same row size, is still buffered, and has slot capacity;
  - otherwise create a new row-page version `2` packed page with one slot.
- Return marked row references for all packed writer rows, including slot `0`.
- Capture buffered-page undo before mutating an existing packed page so
  rollback to a nested savepoint restores the previous slot count and payload.
- Clear the packed append cache when the append buffer flushes, trims, or is
  discarded. A later insert can start a new packed page; it does not need to
  recover old flushed pages in this slice.
- Keep indexed inserts, oversized/BLOB rows, and non-buffered direct appends on
  the existing legacy one-row page writer. Indexed packed appends are a
  follow-up after the no-index slot append and savepoint rollback behavior is
  proven.
- Update buffered row-page checksum refresh to include `row_size * row_count`
  so dirty packed pages flush with the correct checksum.

## Affected Subsystems

- Active row insert append buffer.
- Statement/savepoint rollback for buffered page mutations.
- Direct row reads, scans, counts, and row-state visibility through existing
  packed readers.

## Compatibility Impact

No SQL-visible behavior change is intended. Handler row references remain
opaque eight-byte values. SQL row order for packed pages follows physical page
order and slot order inside the page, matching append order for rows packed
into the same page.

## Single-File And Lifecycle Impact

Packed rows remain durable pages inside the primary `.mylite` file. No new
sidecar, lock, WAL, or temporary file is introduced. The packed append cache is
process memory owned by the active storage checkpoint.

## Public API And File-Format Impact

No public API signature change. Production inserts can now emit row-page
version `2` for fixed-size inline rows written through an active append buffer.
Legacy version `1` row pages remain valid and are still used for oversized rows
and non-buffered direct appends.

## Storage-Engine Routing Impact

No routing policy change. Routed durable tables that reach the active append
buffer can receive packed row pages regardless of whether the SQL requested
`ENGINE=InnoDB`, MyISAM, Aria, or omitted/default MyLite storage.

## Binary-Size Impact

Small first-party storage code only. No dependency change.

## Tests And Verification Plan

- Add storage coverage for three fixed-size active-statement inserts without
  indexes:
  - all returned row ids are marked packed references;
  - scans/counts see the rows before and after commit;
  - the committed file grows by one row page instead of one row page per row.
- Add savepoint rollback coverage:
  - append a row to a packed page;
  - append another row to the same packed page inside a nested savepoint;
  - roll back the savepoint and verify the second row is gone before and after
    commit.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`

## Acceptance Criteria

- Active fixed-size inserts can share one version-2 packed row page.
- Marked row ids from the production no-index insert path work for direct
  reads, scans, counts, and delete visibility.
- Nested savepoint rollback removes packed slots added after the savepoint.
- Legacy direct appends, indexed inserts, and oversized rows keep their
  existing behavior.
- Existing storage and routed storage-engine tests pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed.

## Risks And Unresolved Questions

- This slice only packs rows while the target page remains in the active append
  buffer. Reopening flushed or committed packed pages for free-space reuse is a
  separate pager/free-space-management problem.
- BLOB/TEXT and variable-size packed layouts remain out of scope.
- Indexed packed appends need a follow-up slice to thread the predicted marked
  row id through append-only index pages and maintained root/branch planning.
