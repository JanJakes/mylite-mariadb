# Dirty Page Buffer Single Pressure Eviction

## Problem

Prepared-insert evidence after the dirty-page buffer flush counters showed that
buffer-limit pressure, not root statement commit, drives dirty-page publication:

- `buffer-limit`: `5,009` flushes, `80,144` pages.
- `statement-commit`: `1` flush, `1` page.
- dirty-page-flush checksum refreshes: `79,828`.
- insert-loop dirty refreshes: `91,016`.

The buffer currently flushes all dirty pages when the fixed window is full.
That publishes pages that may still be hot in the same prepared insert loop,
forcing later rewrites to rebuffer and refresh checksums again.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `buffer_dirty_page_for_pager_write()` buffers maintained index leaf, root,
  and branch page rewrites after `capture_dirty_page_undo_for_pager_write()`
  has recorded the in-memory rollback image and ensured the recovery or
  transaction journal protects the page.
- `store_dirty_page_in_buffer()` currently calls
  `flush_statement_dirty_page_buffer()` when
  `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT` is reached. That flush writes every
  buffered page and clears the buffer.
- `flush_statement_dirty_page_buffer()` also serves root statement commit and
  test-hook coverage, so those whole-buffer flush semantics must remain.

## Design

Keep whole-buffer flush behavior for root statement commit and direct test
hooks. Change only buffer-limit pressure:

- add a dirty-page buffer eviction cursor;
- when the buffer is full and a new page must be stored, flush exactly one
  buffered entry at the cursor instead of clearing the entire buffer;
- overwrite that slot with the new dirty page and advance the cursor modulo
  the live buffer count;
- keep existing update-in-place behavior when the page is already buffered; and
- keep dirty-page flush counters so pressure evictions are visible as one-page
  buffer-limit flush events.

The change preserves the fixed 16-page dirty buffer and does not change the
journal file format. It only keeps the other 15 buffered pages in memory across
pressure events.

## Affected Subsystems

- MyLite storage dirty-page buffering.
- Prepared-insert performance benchmark counters.
- Storage test hooks for dirty-buffer lifecycle behavior.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route `ENGINE=InnoDB`
through MyLite storage.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Recovery and transaction
journal companion behavior is unchanged. The slice does not introduce durable
sidecars or change journal encoding.

The safety assumption is that every dirty buffered page has already had its
rollback image captured and journal protection ensured before it enters the
dirty-page buffer. The slice changes which already-protected page is published
under memory pressure, not whether rollback or crash recovery has a saved
original image.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to a small helper and one `size_t` cursor in the internal dirty-page
buffer. No dependency or license change.

## Test And Verification Plan

- Add storage test-hook coverage proving a full dirty-page buffer flushes only
  one page under buffer-limit pressure, keeps the buffer full, and records one
  buffer-limit page flush.
- Keep dirty-buffer checksum copy/flush coverage passing.
- Run the prepared-insert benchmark and compare dirty-page-flush refreshes with
  the prior `79,828` reference.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Buffer-limit pressure flushes one dirty page instead of the whole dirty-page
  buffer.
- Root statement commit and test-hook whole-buffer flush behavior remains
  intact.
- Prepared-insert benchmark evidence shows the new pressure shape and records
  whether dirty-page-flush checksum refreshes improve from the prior `79,828`
  reference.
- Existing storage and embedded storage-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `305.45 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, `32.40 MiB` archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `314.28 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step was `81.300 us/op`, commit was `70.392 ms`
  on this VPS run.

Counter evidence improved despite noisy elapsed timings:

- dirty-page-flush checksum refreshes dropped from `79,828` to `61,049`;
- insert-loop dirty refreshes dropped from `91,016` to `72,595`;
- total zero-tail checksum calls dropped from `242,263` to `223,842`;
- insert-loop zero-tail checksum calls dropped from `132,411` to `113,990`;
- buffer-limit pressure changed from `5,009` flushes / `80,144` pages to
  `61,287` one-page pressure flushes / `61,287` pages; and
- root statement commit remained `1` flush / `1` page.

## Risks

If prepared inserts cycle through more than 16 hot maintained pages with little
reuse, single-page eviction may preserve correctness but not improve elapsed
time. The benchmark counters must decide whether this remains a useful
optimization or only a stepping stone toward a larger dirty-buffer lifetime
change.
