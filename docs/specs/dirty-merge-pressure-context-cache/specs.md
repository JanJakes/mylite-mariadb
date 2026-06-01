# Dirty Merge Pressure Context Cache

## Problem

After maintained-root writer-side decode removal, the prepared-insert component
profile no longer shows redundant maintained-root decode sites. The remaining
protected maintained-root decodes are planning and journal validation work that
must stay in place.

The current profile still reports `31,938` dirty-page merge pressure-context
builds and `19,053` planned fallback stores. A large part of the difference is
broad-victim direct-write classification: the merge guard builds the parent
dirty-buffer pressure context to identify the pressure victim, then direct-writes
the incoming child page and leaves the parent dirty buffer unchanged. Consecutive
direct-write candidates can therefore reuse the same parent pressure context
until a fallback store mutates the parent dirty buffer.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is involved.
- `merge_dirty_page_buffer()` iterates child dirty-buffer entries and delegates
  broad future-current leaf decisions to
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()`.
- `init_dirty_page_buffer_merge_broad_victim_guard_context()` calls
  `dirty_page_buffer_merge_pressure_context()` to compute the flush index and
  maximum resident leaf page id for the full parent dirty buffer.
- `direct_write_dirty_page_buffer_merge_entry()` publishes a child entry to the
  durable page without storing it in `parent->dirty_pages`.
- `store_dirty_page_in_buffer_with_planned_pressure_flush_index()` and
  `store_dirty_page_in_buffer()` mutate `parent->dirty_pages`, so a cached
  pressure context must be invalidated after any fallback store.

## Design

Carry one cached parent pressure context inside the merge loop:

- extend the merge pressure-context plan with an optional cached context;
- initialize each child-entry plan from the cached context when the previous
  entry left one valid;
- have `dirty_page_buffer_merge_pressure_context()` return the cached context
  without rebuilding when one is supplied;
- copy any built or reused context back to the merge-loop cache after the guard
  returns; and
- clear the cache immediately after any successful fallback store into the
  parent dirty buffer.

The cache lifetime is intentionally limited to one `merge_dirty_page_buffer()`
call. It only spans entries that do not mutate `parent->dirty_pages`, and it is
never used across statement boundaries, journal validation, recovery, or durable
page verification.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata behavior, transaction semantics, or error
surface changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, sidecar, lock, or embedded lifecycle
change. Direct-written pages still pass through the existing checksum refresh
and publication path, and fallback stores still use the same pressure-victim
selection result.

## Safety Boundary

Planning and journal validation continue to decode and validate protected pages.
This slice does not remove maintained-root reads, row-page validation,
checksum verification, or recovery-journal page validation. It only reuses a
parent dirty-buffer pressure context while the parent dirty buffer is provably
unchanged.

## Test And Verification Plan

- Update the existing broad-victim direct-write self-test with two consecutive
  direct writes so it asserts one pressure-context build and zero planned
  stores.
- Preserve the planned-store self-test that asserts one context build and one
  planned store for the fallback path.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `315.97 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `333.30 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, archive size `33,993,458` bytes, `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. The repeated direct-write self-test now proves one pressure-context
  build for two broad-victim direct writes, but the prepared-insert structural
  profile stayed at `31,938` merge pressure-context builds and `19,053`
  planned stores. The sampled prepared insert step was `81.707 us/op`; full
  page checksum calls stayed `8`, zero-tail checksum calls stayed `127,063`,
  dirty `index-leaf` merge direct writes stayed `66,144`, dirty leaf pressure
  admissions stayed `21,031`, index-leaf dirty refreshes stayed `87,176`, and
  maintained-root decode sites stayed protected-only:
  `read_index_leaf_run_root` (`1`), `plan_maintained_index_root_inserts` (`2`),
  and `validate_recovery_journal_saved_page` (`2`).

## Acceptance Criteria

- Consecutive broad-victim direct writes reuse the parent pressure context.
- Fallback stores still use a planned pressure flush index when the guard built
  a context for that entry.
- Parent dirty-buffer mutation invalidates the cache before the next child
  entry.
- Prepared-insert maintained-root decode sites remain protected planning and
  journal-validation sites only.

## Risks

- The cache is only correct while `parent->dirty_pages` is unchanged. The
  implementation invalidates after every fallback store to keep that invariant
  explicit.
- This is a storage hot-path optimization, not a new SQL compatibility feature.
