# Branch Tail Overlay Cache Owner

## Problem

After retaining more branch-tail overlay cache entries, a long
`prepared-insert-components` profile still shows the prepared insert step under
`index_branch_tail_has_live_overlay()` and `read_page_at()`.

The remaining issue is cache lifetime. Prepared executions inside a durable SQL
transaction can run as nested storage checkpoints. The branch-tail overlay check
currently stores entries on `active_statement_for_file(file)`, which can be the
nested statement. That cache is freed when the nested statement commits, so the
next prepared execution in the same transaction cannot reuse the verified
branch-tail range.

Other active caches use the root cache owner through
`active_cache_statement_from_statement()`. Branch-tail overlay caches should use
the same ownership model, with rollback clearing parent caches to avoid stale
entries from aborted nested work.

## Source Findings

- Base line: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB refs:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()` dispatches
    accepted inserts to `table->file->ha_write_row()`.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` wraps the handler
    `write_row()` call.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` calls
    `mylite_storage_append_row_with_index_entries()` for durable MyLite rows.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:index_branch_tail_has_live_overlay()`
    currently asks `active_statement_for_file(file)` for cache storage.
  - `packages/mylite-storage/src/storage.c:active_cache_statement_from_statement()`
    returns the cache owner that survives nested statements in the active
    checkpoint chain.
  - `packages/mylite-storage/src/storage.c:mylite_storage_rollback_statement()`
    clears parent exact-index, live-row, row-state, live-row-id, row-payload,
    and buffered-update caches after nested rollback.

## Design

- Add a small helper that resolves the branch-tail overlay cache owner from the
  active statement for a file.
- Store and read branch-tail overlay cache entries from that cache owner, not
  necessarily the current nested statement.
- Add branch-tail overlay caches to the parent-chain clears performed after a
  nested rollback.
- Keep commit behavior simple: cache entries written to the root cache owner are
  already available after nested commit.

The rollback clear is deliberately conservative. It avoids stale present-overlay
or over-advanced absent entries if a nested statement that populated the root
cache later rolls back.

## Compatibility Impact

No SQL, public C API, metadata, storage-routing, or file-format behavior
changes. The slice only changes in-memory planning cache lifetime.

## Single-File And Embedded Lifecycle

The cache remains statement/transaction-local scratch state. It is freed with
the root statement or cleared on rollback. No primary-file or companion-file
behavior changes.

## Public API, Binary Size, And Dependencies

No public API or dependency impact. Binary-size impact is limited to one helper,
one parent-chain clear function, and a focused test hook.

## Tests And Verification Plan

- Add a storage test hook proving branch-tail cache lookup resolves to the root
  active cache owner when the current active checkpoint is nested.
- Keep the over-limit cache-retention hook passing.
- Run:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Acceptance Criteria

- Branch-tail overlay cache entries are owned by the root active cache owner.
- Nested rollback clears parent branch-tail overlay caches.
- Existing row-state overlay safety behavior remains unchanged.
- Relevant storage and storage-smoke checks pass.
- Prepared-insert profiling shows reduced `index_branch_tail_has_live_overlay()`
  read time or records the next bottleneck.

## Verification

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  158.52 seconds.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure` passed in
  36.14 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` reported prepared insert step
  at 91.813 us/op.
- A longer local profile,
  `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components --profile-iterations=200000 1000`,
  reported prepared insert step at 136.412 us/op. The sample no longer showed
  `index_branch_tail_has_live_overlay()` as the dominant stack; remaining time
  was mostly under branch leaf planning, especially `decode_index_leaf_page()`
  and checksum work while scanning candidate redistribution leaves.

## Risks

- Root-owned present-overlay cache entries from nested work would be stale after
  rollback without the new parent-chain clear.
- Root-owned absent entries can be over-advanced by nested work; clearing parent
  branch-tail caches on rollback handles that conservatively.
