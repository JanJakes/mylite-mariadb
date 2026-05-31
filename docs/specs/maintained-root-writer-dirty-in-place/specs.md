# Maintained Root Writer Dirty In-Place Updates

## Problem

The prepared-insert profile still reports `674` `index-root` direct-read dirty
copies after maintained-root planning began borrowing dirty root pages by const
pointer. Those remaining copies are writer-side reads of root pages that are
already resident in the current statement dirty buffer. The writer immediately
mutates the copied page and stores it back into the same dirty-buffer entry,
where existing fast replacement helpers prove the byte shape.

The safe slice is to update the current statement's protected dirty root entry
in place for planned single-page root inserts and overflow-tail marks, and to
borrow the dirty root by const pointer for overflow promotion validation. Parent
dirty-buffer entries and unprotected pages must keep the existing copy/fallback
path.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc::write_row()` routes durable row inserts
  into `mylite_storage_append_row_with_index_entries()` after MariaDB handler
  duplicate-key, autoincrement, generated-row, and FK checks.
- The affected implementation is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source changes are needed.
- `write_maintained_index_root_inserts()` calls
  `read_maintained_index_root_dirty_page()`, mutates the copied page with
  `insert_maintained_index_root_entry()`, and writes it back through
  `pager_write_buffered_maintained_index_page()`.
- `write_maintained_index_root_overflow_flags()` follows the same read/mutate/
  write-back shape for `mark_maintained_index_root_overflow_tail()`.
- The current planner already carries the validated root entry count and insert
  position, and the journal setup protects maintained-root pages before writer
  mutation.
- Nested statements must not mutate a parent dirty-buffer entry directly; a
  parent resident page still needs the existing copy into the child statement's
  dirty buffer.

## Design

Add narrow writer helpers that only act when:

- the active statement for the `FILE *` can either use its own dirty-buffer
  entry for the target root page or clone a parent dirty-buffer entry into the
  current statement;
- the page is a normal 4 KiB page and is not an append-buffer page;
- the existing planned root insert or overflow-tail validation succeeds against
  the resident bytes.

For planned single-page root inserts, mutate the current statement's
`entry->page` directly with
`insert_maintained_index_root_entry(..., refresh_checksum=0)` and mark the
dirty entry checksum-dirty. If the entry already belonged to the current
statement, record the same replacement/fast-path test-hook evidence as the old
copy-and-store path. If the entry was cloned from a parent dirty buffer, leave
replacement evidence to the later child-to-parent merge, matching the previous
counter shape.

For overflow-tail marks, use the same current-entry or parent-clone flow,
mutate with `mark_maintained_index_root_overflow_tail(..., refresh_checksum=0)`,
and mark the current entry checksum-dirty.

If any precondition fails, keep the existing
`read_maintained_index_root_dirty_page()` fallback so durable reads and
direct-write fallbacks remain unchanged. Parent entries are never mutated in
place; the helper always creates or uses a current-statement child copy first.

Overflow promotion only validates the dirty root bytes before publishing a
branch snapshot, so it can use the existing const dirty-root planning read
helper instead of copying the page into a mutable scratch buffer first.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol behavior, durable file-format bytes, or
transaction semantics change.

## Single-File And Lifecycle Impact

No file lifecycle, sidecar, lock, journal format, recovery format, or embedded
startup/teardown change. The slice relies on existing journal/undo protection
and leaves dirty root pages checksum-dirty until the existing publication path
refreshes them.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, license, or expected binary-size
change. Test-hook benchmark output should show fewer `index-root` direct-read
dirty copies while preserving maintained-root decode and checksum counts.

## Tests And Verification Plan

- Add focused storage self-tests for:
  - planned root insert writer clones a parent dirty-buffer root into the
    current statement, updates the child copy in place, does not record an
    `index-root` direct-read dirty copy, leaves the parent bytes unchanged, and
    preserves the checksum-dirty root decode contract;
  - overflow-tail writer marks a current dirty-buffer root in place, does not
    record an `index-root` direct-read dirty copy, and preserves the planned
    overflow root state.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert benchmark output shows `index-root` direct-read dirty copies
  reduced from `674` to `0` while maintained-root decode sites remain at the
  protected validation count.
- Full-page checksum calls, zero-tail checksum calls, dirty refresh counts, and
  dirty-buffer replacement evidence remain equivalent.
- Storage and embedded storage-engine smoke verification pass.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test` passed.
- `build/dev/packages/mylite-storage/mylite_storage_test` passed.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  `312.17 sec`.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `321.12 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed; `libmariadbd.a` was `33,983,346` bytes with `478` members.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed. The benchmark reported `0` `index-root` direct-read dirty copies,
  down from `674`. Maintained-root decode sites stayed at `677`, full-page
  checksum calls stayed at `8`, zero-tail checksum calls stayed at `227,063`,
  and `index-root` dirty-buffer replacements stayed at `668`. The sampled
  prepared insert step was `77.087 us/op` on the final run, so the result is
  recorded primarily as a structural copy-elision win.

## Risks

- In-place mutation must be limited to the current statement's dirty-buffer
  entry. Mutating a parent entry would break nested-statement rollback
  isolation.
- Parent dirty-buffer cloning still copies one page into the child statement;
  the slice removes the extra dirty-read stack copy and preserves child
  rollback isolation, but it is not a zero-copy parent mutation.
