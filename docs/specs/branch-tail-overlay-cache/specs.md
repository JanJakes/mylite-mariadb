# Branch Tail Overlay Cache

## Problem

Maintained branch-root insert planning calls `index_branch_tail_has_live_overlay()`
before it can split, redistribute, promote, or refold branch leaves. The scan is
correct but expensive in prepared insert transactions after many pages have been
appended beyond the branch children. A local sample of
`mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000`
showed the branch-tail overlay check repeatedly reading the same unchanged tail
pages inside `plan_branch_index_root_insert()`.

## Source Findings

- Base line: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB routes row mutations through handler calls:
  `mariadb/sql/handler.cc` wraps `handler::write_row()`,
  `handler::update_row()`, and `handler::delete_row()` through
  `ha_write_row()`, `ha_update_row()`, and `ha_delete_row()`.
- MyLite's handler forwards durable row mutations to first-party storage in
  `mariadb/storage/mylite/ha_mylite.cc` through
  `mylite_storage_append_row_with_index_entries()`,
  `mylite_storage_update_row_with_index_entries()`, and
  `mylite_storage_delete_row()`.
- The hot scan is first-party storage code in
  `packages/mylite-storage/src/storage.c`. It walks pages after the largest
  child page id in the current branch root and treats matching index-entry or
  row-state pages as a live append-tail overlay.

## Design

Add a bounded active-statement cache for branch-tail overlay checks:

- key by table id, index number, key size, branch level, and the largest branch
  child page id seen by the scan;
- store whether a matching live overlay was found and the page count scanned;
- reuse an absent result only when the cached max-child page id is not greater
  than the current max-child page id, then scan only the unverified suffix;
- reuse a present result only when the cached overlay page still sits after the
  current max-child page id;
- ignore allocation failure and fall back to the existing scan result.

The cache lives only on `mylite_storage_statement`. It is cleared with the rest
of active statement state and is never promoted to durable caches.

## Scope

- Optimize branch-tail overlay planning scans for active storage statements.
- Add test hooks that count real overlay scan passes and scanned tail pages.
- Add regression coverage proving repeated planning checks reuse the cache.

## Non-Goals

- No file-format change.
- No durable/process-global branch-tail cache.
- No change to branch split, redistribution, promotion, or refold semantics.
- No broader pager rewrite or checksum batching in this slice.

## Compatibility Impact

No SQL-visible behavior changes. MySQL/MariaDB compatibility stays governed by
the same MariaDB handler mutation flow and MyLite storage visibility rules.

## Single-File And Embedded Lifecycle

The cache is process-local and statement-owned. It adds no companion files and
does not change rollback-journal, transaction-journal, or recovery behavior.
Rollback and close paths discard the cache with the active statement.

## Public API, File Format, And Storage Routing

No public API or file-format impact. Storage-engine routing remains unchanged.
The optimization applies after a table is already routed to MyLite storage.

## Binary Size And Dependencies

The slice adds a small first-party cache structure and no dependencies.

## Tests And Verification

- Add storage test hooks for branch-tail overlay scan count and scanned page
  count.
- Extend existing branch-root tests to assert that repeated planning checks
  consult the cache instead of rescanning the same tail.
- Run:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Acceptance Criteria

- The existing overlay scan semantics remain unchanged on cache miss.
- Active repeated branch-tail checks reuse verified cached tail state.
- Allocation failure in cache maintenance cannot turn a valid storage operation
  into an error.
- Storage and embedded storage-engine tests pass.
- The prepared insert component benchmark does not regress locally.

## Verification

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in 155.32 sec.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed in 30.94 sec.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`:
  passed; prepared insert step component measured 107.223 us/op locally.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.

## Risks

- A too-broad cache key could hide append-tail row-state or index-entry pages.
  The cache therefore uses the branch max-child page id as the tail boundary and
  rescans when the current branch shape cannot be proven covered.
- Reusing a present overlay after a branch grows past the overlay page would be
  conservative but could suppress maintenance unnecessarily. The present result
  is used only while the overlay page remains beyond the current max child.
