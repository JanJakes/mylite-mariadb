# Inline Maintained Index Plan Storage

## Problem

Prepared primary-key update profiling still spends time in storage update
planning after handler-level index validation was reduced. Maintained index
root planning allocates tiny per-statement arrays for current routed tables even
though root mutation planning is already bounded by the rollback journal's
protected-page limit.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `packages/mylite-storage/src/storage.c` limits dirty maintained-root
  protection through `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES`.
- `plan_maintained_index_root_inserts()` allocates `index_entry_changed` and
  grows a dynamic insert-plan entry array even when only one or two maintained
  roots are present.
- `plan_maintained_index_root_updates()` has the same dynamic changed-entry
  copy and update-plan entry growth.
- `append_maintained_index_*_plan_entry()` cannot validly append more entries
  than the protected-page limit because each maintained root needs journal
  protection before it can be dirtied.

## Design

- Add inline plan storage for maintained index root insert and update entries.
- Use inline `index_entry_changed` storage when the caller's index-entry count
  fits the journal protected-page bound.
- Fall back to heap allocation for unusually many index-change flags so the
  public storage API contract remains unchanged.
- Keep the existing journal protection, root mutation, append-tail fallback,
  duplicate-key, and active-cache behavior unchanged.

## Scope

In scope:

- First-party storage maintained-root insert/update plan allocation strategy.
- Prepared update performance evidence.

Out of scope:

- Multi-page navigable index roots.
- Changing index-entry API shapes.
- Changing root overflow or transactional journal semantics.

## Compatibility Impact

No SQL, public C API, file-format, or storage-engine routing behavior changes.
The same changed-entry bitmap is passed to downstream write and cache
maintenance code.

## Single-File And Lifecycle Impact

No durable file lifecycle change. Dirty maintained-root pages remain protected
by the existing recovery journal before in-place mutation.

## Test Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- Existing maintained-root insert, update, delete, rollback, and routed-engine
  tests pass.
- Hot insert/update plans with no more than the journal protected-page bound
  avoid heap entry-array allocation.
- Hot update plans with no more than that many index-entry flags avoid heap
  changed-array allocation.
- Larger index-entry counts still allocate and free heap storage correctly.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: no
  changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `2.457 us/op` in the final direct run.
- A two-second macOS `sample` run over the same phase did not show maintained
  index plan entry allocation frames after scalar-only plan cleanup; the
  remaining sampled storage cost stayed in active update rewrite, undo capture,
  table-id lookup, and MariaDB quick range planning/explain bookkeeping.
