# Empty Index Cursor Reset Fast Path

## Problem

Prepared point-update execution clears the MyLite handler index cursor around
cursor setup and teardown. After null-free guards, an already-empty cursor no
longer calls the storage free wrapper, but `clear_index_cursor()` still rewrites
every cursor state field on no-op `index_init()` / `index_end()` paths.

Sampling still shows `ha_mylite::clear_index_cursor()` in the prepared-update
handler stack.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  the first-party MyLite handler.
- `ha_mylite::index_init()` and `ha_mylite::index_end()` call
  `clear_index_cursor()` even when no cursor has been built.
- `ha_mylite::build_index_cursor()` starts by clearing the previous cursor and
  may also leave a logical no-row cursor by setting `index_cursor_number` and
  `index_cursor_filtered` while the buffer pointers remain `NULL`.
- `clear_index_cursor()` owns the reset from any populated or logical cursor
  state back to the empty `MAX_KEY` sentinel.

## Design

- Return early from `clear_index_cursor()` only when all cursor buffer pointers
  are `NULL`, the cursor sizes and indexes are zero, `index_cursor_number` is
  `MAX_KEY`, and the cursor is not filtered.
- Preserve the existing free and reset path for every populated cursor and for
  logical no-row cursors that still remember a key/filter state.
- Keep inline-buffer ownership checks unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The handler releases the same owned buffers and still resets
all non-empty cursor states.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only skips redundant handler field stores when cursor state is
already empty.

## Tests And Verification

- Run:
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Already-empty index cursor cleanup returns without rewriting cursor state.
- Logical no-row cursors still reset to the empty sentinel when explicitly
  cleared.
- Existing routed storage, cursor, transaction, rollback, and embedded
  storage-engine tests pass.

## Risks

- The fast path must not skip logical cursor reset after a not-found lookup.
  The guard includes `index_cursor_number == MAX_KEY` and
  `!index_cursor_filtered` so keyed no-row cursor state is still reset.
