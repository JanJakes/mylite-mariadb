# Merge Future Relations From Guard

## Problem

Prepared-insert test-hook runs still duplicate future-page relation work during
dirty-page buffer merges. The production merge guard already classifies a child
dirty page against the parent header and append buffers before deciding whether
the page can be direct-written. The test-hook future relation table then
recomputes the same header relation and parent/child append-buffer relation for
each future page.

The current smoke profile reports `122,388` future-current `index-leaf` merge
rows in that attribution table, all `within-current-header` and all outside
append buffers. Rechecking both append buffers for those rows is benchmark-path
overhead, not storage behavior.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test-hook attribution in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is involved.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
  checks `parent->has_current_header`, `parent->current_header`, and
  parent/child append buffers while classifying future dirty pages.
- `record_dirty_page_buffer_merge_future_page_relations()` currently calls
  `dirty_page_buffer_merge_future_header_relation_for_entry()` and
  `dirty_page_buffer_merge_future_append_relation_for_entry()` after the guard
  has returned.
- Future relation counters are diagnostic only. Production merge policy,
  dirty-buffer pressure selection, journal protection, checksum publication,
  and durable page bytes do not depend on them.

## Design

Carry test-hook future relation state out of the merge guard:

- add a test-hook-only relation struct with header and append relation slots;
- initialize it to invalid for each merge entry;
- when the guard receives a page beyond the parent statement header, fill the
  relation slots before any future-page early return, including the
  parent-not-full path; and
- record the future relation counters from that carried state instead of
  recomputing both relation helpers after the guard returns.

Do not change guard outcomes, direct-write policy, dirty-buffer fallback
admission, pressure-victim planning, checksum timing, maintained-root planning,
recovery-journal validation, or file format.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, metadata, transaction semantics, or error-surface
changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change.

## Test And Verification Plan

- Reuse the existing future-page relation self-test, which covers parent-only,
  child-only, shared, no-append, within-current-header, and past-current-header
  relation counters.
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

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `316.12 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `329.13 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, archive size `33,989,146` bytes, `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step sampled `73.126 us/op`; structural counters stayed at
  `8` full-page checksum calls, `227,063` zero-tail checksum calls, `677`
  maintained-root decodes, `31,938` merge pressure-context builds, `19,053`
  planned stores, `21,031` dirty leaf pressure admissions, `66,144` merge
  direct writes, `87,176` index-leaf dirty refreshes, and `122,388`
  future-page relation rows (`within-current-header` / append relation `none`).

## Acceptance Criteria

- Future-page relation counters remain structurally unchanged.
- The recorder no longer recomputes parent/child append-buffer relations after
  the merge guard already classified the future page.
- Prepared-insert maintained-root decode and checksum counters remain
  structurally unchanged.

## Risks

- This is a benchmark/test-hook source-path cleanup, not a product semantic or
  durability improvement.
