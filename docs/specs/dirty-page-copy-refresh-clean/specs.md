# Dirty Page Copy Refresh Clean

## Problem

The dirty-refresh source counters show that prepared inserts still refresh
dirty-page-buffer pages both when they are copied for generic reads and again
when the same buffered page is flushed:

- dirty-page flush refreshes: `121,271`
- dirty-page copy refreshes: `7,828`
- prepared insert step: `75.706 us/op`

Append-buffer copy already refreshes the buffered original and clears its
checksum-dirty flag before copying. Dirty-page copy refreshes only the copied
scratch page and leaves the buffered original dirty, forcing a later flush to
refresh the same page again when no intervening write dirtied it.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- MyLite dirty page reads flow through
  `packages/mylite-storage/src/storage.c::read_page_at()`,
  `copy_dirty_page_buffer()`, and `refresh_dirty_buffered_page_checksum()`.
- `copy_buffered_append_page()` refreshes the buffered append page in place,
  clears the append-buffer checksum-dirty slot, and then copies the clean page.
- `copy_dirty_page_buffer()` currently copies the dirty-page buffer entry,
  refreshes the scratch copy, and leaves the buffer entry's checksum-dirty flag
  set.
- Dirty-page buffer flush in `flush_statement_dirty_page_buffer()` refreshes
  entries whose checksum-dirty flag is still set before durable publication.

## Design

Change `copy_dirty_page_buffer()` to mirror append-buffer copy semantics:

- use a mutable dirty-page buffer entry while walking the active statement
  chain;
- when the entry is checksum-dirty, refresh the buffered entry in place;
- clear the entry's checksum-dirty flag after a successful refresh;
- copy the now-clean buffered page into the caller's scratch page; and
- keep later writes responsible for setting the checksum-dirty flag again.

This removes duplicate refresh work for pages that are read before flush and
then published without another mutation. It does not change page content except
for publishing the checksum field earlier on the existing dirty buffered page.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route through the
same MyLite storage engine for `ENGINE=InnoDB`.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. Dirty pages still publish valid checksums before durable file writes
or checksum-validating generic reads.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to a small storage helper change and test adjustments. No dependency or
license change.

## Test And Verification Plan

- Update dirty branch-page and dirty index-leaf buffer tests to assert that
  copy-for-read refreshes the buffered original and clears the checksum-dirty
  flag.
- Assert that a later dirty-page flush does not perform another dirty-page
  flush refresh when the entry was already cleaned by copy-for-read.
- Run the prepared-insert benchmark and record the dirty-page copy and flush
  source split.
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

- Dirty-page copy-for-read refreshes checksum-dirty buffered entries in place.
- Dirty-page copy-for-read clears the buffered entry's checksum-dirty flag.
- A copied-clean dirty-page entry flushes without another dirty-page flush
  refresh unless a later write dirties it again.
- Existing storage and embedded storage-engine tests pass.

## Verification

Verified on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `1/1` test in `293.33 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `2/2` tests in `305.48 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step measured `90.186 us/op`, while the one-shot
  prepared insert commit measured `46.892 ms`. Total zero-tail checksum calls
  dropped from the prior `284,426` to `282,296`. Dirty-page copy refreshes
  dropped from `7,828` to `5,790`, while dirty-page flush refreshes remained
  nearly flat at `121,179`, indicating that most flush refreshes are from pages
  not cleaned by copy-for-read or from pages dirtied again after copy.

## Risks

The checksum field becomes valid earlier in the dirty buffer. This is intended,
but any later mutation must continue to set checksum-dirty so the page is
refreshed again before reads or publication.
