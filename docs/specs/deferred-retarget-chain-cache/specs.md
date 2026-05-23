# Deferred Retarget Chain Cache

## Problem

Prepared row-only updates now skip writing a child deferred durable-cache
retarget marker when an ancestor statement already has a same-table or
all-tables marker. The hot update path still calls
`statement_chain_has_deferred_durable_cache_retarget()` for every active
in-place rewrite, and that helper walks the active statement's parent chain to
rediscover the same ancestor marker.

A 2026-05-23 local sample of
`prepared-row-only-update-components --profile-iterations=10000000 10000`
still showed that chain helper below
`update_row_with_index_entries_for_context()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `initialize_checkpoint_statement()` creates nested storage statements from an
  active parent and calls `clone_parent_checkpoint_snapshot()` before row-DML
  work begins.
- `defer_durable_cache_retarget_after_table_mutation()` records a local
  deferred durable-cache marker on the current statement.
- `merge_deferred_durable_cache_retarget()` folds a committed child marker into
  the parent.
- `statement_chain_has_deferred_durable_cache_retarget()` currently scans the
  current statement and all parents to determine whether a mutation is already
  covered by a deferred retarget marker.
- Existing covered-marker tests prove that a second nested row-only rewrite
  under a transaction can skip its child marker when the parent marker already
  covers the table.

## Design

Cache inherited deferred durable-cache retarget coverage on each nested
statement at statement initialization:

- record whether the parent chain already has a marker,
- record whether that inherited marker covers all tables,
- record the inherited table id for same-table coverage,
- mark inherited coverage as ambiguous when multiple table-specific markers are
  present in ancestors.

`statement_chain_has_deferred_durable_cache_retarget()` should then check the
current statement's local marker and inherited snapshot directly instead of
walking parent links. Local markers stay authoritative for mutations made by
the current statement. The inherited snapshot is only a shortcut for markers
that existed before the nested statement began.

Ambiguous inherited coverage must not be treated as all-table coverage. The
common same-table path keeps a direct inherited table-id check, while ambiguous
multi-table inheritance falls back to scanning parent-local markers when the
direct inherited table id does not match. That preserves the previous answer
for unrelated tables without putting the parent scan back on the covered
single-table prepared-update path.

This is safe because a parent statement does not mutate independently while a
child statement is active. Child commit still uses the existing merge helper,
and child rollback still discards child-local state.

## Affected Subsystems

- MyLite storage statement lifecycle.
- Deferred durable-cache retarget marker lookup.
- Prepared row-DML storage performance.

## Compatibility Impact

No SQL, C API, storage-engine routing, DDL metadata, or file-format behavior
changes. The optimization only avoids repeated parent-chain scans for existing
transient marker state.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, sidecar, or recovery change. The cached marker
coverage is process-local statement metadata and is cleared with statement
lifecycle cleanup.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Binary-Size, License, And Dependency Impact

Small first-party storage fields and helper logic only. No dependency or
license change.

## Test And Verification Plan

- Extend the existing covered deferred-retarget marker storage test so a child
  nested under an already-covered parent still skips its local marker.
- Add a marker-scope regression test proving inherited markers for two
  different table ids do not falsely cover an unrelated table id.
- Build `mylite_storage_test` and the storage-smoke targets.
- Run focused storage tests and full storage-smoke CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run the focused prepared row-only update component benchmark as noisy local
  regression evidence.

## Completed Verification

On 2026-05-23:

- `git diff --check` passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c` passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=10000000
  10000` reported `1.350 us/op` for the prepared row-only update step
  component in the unsampled run.
- A 2-second sample of the same phase written to
  `/tmp/mylite-deferred-retarget-chain-cache-final.sample.txt` no longer showed
  `statement_chain_has_deferred_durable_cache_retarget()` or the ambiguous
  fallback scanner as named frames.

## Acceptance Criteria

- A nested statement can answer same-table and all-tables inherited durable
  retarget coverage without scanning parent links.
- Ambiguous inherited multi-table coverage preserves previous parent-chain
  semantics for unrelated tables.
- Existing local marker behavior, nested commit merge, nested rollback discard,
  and all-table escalation remain unchanged.
- Existing storage and embedded storage-engine tests pass.
- Prepared row-only update profiling no longer shows repeated parent-chain
  walking as a visible storage bookkeeping frame on the covered single-table
  path.

## Risks

- Stale inherited coverage would be unsafe if parent markers could change while
  a child is active. The storage statement stack is single-active-child, so the
  parent cannot run independent row-DML until the child commits or rolls back.
