# Update Row Foreign Key Presence Reuse

## Problem

`ha_mylite::update_row()` can ask the handler twice whether the current table is
referenced by parent foreign keys during one row update. The first lookup gates
same-row self-reference update actions; the second gates broader parent action
and parent-row validation work after duplicate-key and child-key checks.

The handler already caches parent-FK presence across calls, so the second lookup
usually returns from a cached field. It still repeats the session
`foreign_key_checks` guard, an epoch load, a function call, and output setup in
the prepared update hot path. The result cannot change inside the same row
update unless table metadata changes, which row-DML does not do.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` calls
  `has_parent_foreign_keys()` before preparing index entries, then calls it
  again after duplicate-key and child-FK checks.
- `ha_mylite::has_parent_foreign_keys()` already invalidates its cached result
  when `mylite_foreign_key_presence_epoch` changes.
- Row-DML paths update row, row-state, index, autoincrement, and volatile row
  state; they do not publish foreign-key metadata or increment the FK presence
  epoch.
- Session `foreign_key_checks` is a statement/session setting. It is not
  expected to change in the middle of one handler `update_row()` call.

## Design

- Compute the row update's `check_foreign_keys` boolean once after the primary
  file has been resolved.
- Resolve `has_parent_foreign_keys()` once when FK checks are enabled, before
  same-row action handling.
- Reuse that parent-presence result for the later parent action and validation
  block after duplicate-key and child-FK checks.
- Keep child-FK presence as a separate lookup because it describes constraints
  owned by the updated table, not constraints referencing it.
- Keep the existing FK presence cache and epoch invalidation unchanged.

## Compatibility Impact

No SQL, public C API, metadata, file-format, or routing behavior changes.
Immediate FK checks and supported action ordering remain the same because the
same parent-presence value is used inside the same row update.

## Single-File And Lifecycle Impact

No durable lifecycle change. The slice only avoids repeated handler metadata
presence resolution before existing row/index/checkpoint publication paths.

## Binary-Size Impact

Negligible. The change removes a repeated call site and adds no dependency.

## Tests And Verification

- Rebuild the MariaDB storage-smoke archive because the handler source changes:
  `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- Rebuild focused first-party targets:
  `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- Run:
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`

## Acceptance Criteria

- `update_row()` calls `has_parent_foreign_keys()` at most once per row update.
- Existing child-FK checks, parent-FK actions, same-row self-reference behavior,
  and parent validation remain covered by the storage-smoke tests.
- No metadata cache invalidation behavior changes.

## Risks And Unresolved Questions

- This assumes row-DML cannot change FK metadata during a single handler update
  call. That matches MyLite's current FK DML implementation and MariaDB's
  handler call boundary.
- Broader FK action graph performance still depends on richer indexed child-row
  traversal; this slice only removes repeated handler presence control work.
