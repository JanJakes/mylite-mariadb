# Full Window Dirty Page Buffer

## Problem

Prepared-insert checksum phase counters show that dirty-page flush refreshes
are not a commit-only cost. They happen in the insert loop:

- insert-loop dirty refreshes: `131,049`
- insert-loop dirty-page flush refreshes: `121,179`
- commit dirty refreshes: `2,773`, all from append-buffer flush

The dirty-page buffer currently flushes when it reaches
`MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES / 2`, which is `8` pages.
The journal protection window is `16` pages. Flushing at half the window makes
hot maintained index pages publish and re-dirty during the same prepared insert
loop more often than the journal limit requires.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- MyLite's durable journal format defines
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES` as `16`.
- The dirty-page buffer limit in `packages/mylite-storage/src/storage.c` is
  currently half of that journal window.
- Dirty maintained index pages capture their undo/journal protection before
  entering `store_dirty_page_in_buffer()`.
- `store_dirty_page_in_buffer()` flushes the dirty-page buffer when the buffer
  count reaches the limit, then stores the next page.

## Design

Use the full journal page window for the dirty-page buffer limit:

- set `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT` to
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES`;
- keep the initial capacity unchanged;
- keep the durable journal format and max protected page count unchanged;
- keep dirty-page flush behavior unchanged once the full window is reached; and
- add a test hook that fills the dirty-page buffer to the full journal window
  and proves the entries remain buffered instead of flushing at the former
  halfway point.

This is still a bounded local buffer. It does not claim unbounded multi-page
transactional index maintenance.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route through the
same MyLite storage engine for `ENGINE=InnoDB`.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. The slice changes only how long already-protected dirty maintained
index pages can remain statement-local before raw file publication.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Runtime memory for a dirty
page buffer can grow from 8 to 16 page entries, roughly one extra 32 KiB window
per active statement that uses the buffer. No dependency or license change.

## Test And Verification Plan

- Add a focused storage test hook that stores
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES` dirty pages and asserts
  they remain buffered.
- Run the prepared-insert benchmark and record dirty-page flush refreshes and
  prepared insert step timing.
- Keep storage and routed embedded storage-engine tests passing.
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

- Dirty-page buffer capacity uses the full journal protected-page window.
- A storage test hook proves the buffer can hold the full window without a
  halfway flush.
- Existing storage and embedded storage-engine tests pass.
- Prepared-insert benchmark output shows whether insert-loop dirty-page flush
  refreshes drop.

## Verification

Verified on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `1/1` test in `347.53 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `2/2` tests in `345.57 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step measured `80.532 us/op`. Total zero-tail
  checksum calls dropped from `282,296` to `242,263`; insert-loop zero-tail
  calls dropped from `172,444` to `132,411`; and dirty-page flush refreshes
  dropped from `121,179` to `79,828`. The local timing was noisy, so this
  slice claims the counter reduction rather than a step-time reduction.

## Risks

The slice intentionally increases per-statement dirty-buffer memory. It does
not increase the durable journal's protected-page capacity, so broader
multi-page maintained-index mutations beyond that window remain out of scope.
