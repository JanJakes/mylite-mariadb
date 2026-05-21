# Unlocked File Error Clear

## Problem

Prepared primary-key updates borrow the same active statement-owned `FILE *`
for repeated point reads and row rewrites. Scope-aware close helpers already
avoid rediscovering ownership, but they still reset the borrowed stream state
with `clearerr()`. On macOS sampling, that path takes the locked stdio route
through `flockfile()` / `funlockfile()` even though MyLite's active statement,
read statement, read snapshot, and cached read file handles are thread-local.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `packages/mylite-storage/src/storage.c` stores active statement, active read
  statement, active read snapshot, transaction-journal snapshot, and read-file
  cache state in `_Thread_local` variables.
- `close_existing_file_scope()`, `close_existing_update_file_scope()`,
  `close_existing_file()`, `take_cached_read_file()`, and `cache_read_file()`
  only need to clear the stream EOF/error indicators before reusing or caching
  these thread-owned handles.
- The sampled prepared-update profile at `5e201d707dd` still shows
  `clearerr()` costs under `close_existing_update_file_scope()` and
  `close_existing_file_scope()`.

## Design

- Add one storage-local helper for clearing a reused `FILE *` error state.
- Use `clearerr_unlocked()` on Apple and Linux builds, where the storage
  module's reused handles are thread-local and the unlocked primitive preserves
  the same EOF/error reset without stdio locking.
- Fall back to standard `clearerr()` on other platforms.
- Replace storage-local borrowed/cached handle clears with the helper. Owned
  close semantics, file locks, snapshots, and journal ownership are unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
semantics change. The slice only changes the implementation used to reset
stdio state on thread-owned storage handles.

## Single-File And Lifecycle Impact

No durable or transient files are introduced. Borrowed statement-owned handles,
read snapshots, transaction-journal snapshots, and cached read handles keep the
same lifetime and close behavior.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 1000 1000000`
  - sampled prepared-update run to confirm borrowed-scope close no longer
    reports locked `clearerr()` as a hot stack
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Borrowed active file scope close and cached read-file reuse clear stream state
  without stdio locking on Apple/Linux builds.
- Non-borrowed owned file close behavior remains unchanged.
- Focused storage and embedded storage-engine tests pass.
- Prepared-update timing does not regress.

## Risks

- `clearerr_unlocked()` must only be used where the storage module owns the
  stream on the current thread. This slice limits the helper to MyLite's
  thread-local active/cached storage handles and retains the locked fallback for
  other platforms.
