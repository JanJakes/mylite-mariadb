# Active Exact Index Cache After Mutation

## Problem

Prepared primary-key updates run as repeated exact-key reads followed by row
updates inside one explicit storage transaction. After the first mutation,
`load_cached_exact_index_entry_in_statement()` refuses to create an active
exact-index cache whenever the mutation statement has a deferred durable-cache
retarget. That is safe for stale durable-cache seeding, but too broad for
active-cache creation: the storage layer then falls back to repeated
append-history exact-index scans for the rest of the transaction.

The previous direct-update profiling slice confirmed that this scan path now
dominates `prepared-update-components`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  reads the target row through `read_exact_unique_index_row_into()` before
  applying the storage update.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::read_exact_unique_index_row_into()`
  uses the active THD storage checkpoint when one exists and calls
  `mylite_storage_find_indexed_row_in_statement_into()`.
- `packages/mylite-storage/src/storage.c::find_exact_index_row_id()` first
  probes an active exact-index cache, then attempts to create one with
  `load_cached_exact_index_entry_in_statement()`, then falls back through
  published leaf roots and append-history scanning.
- `packages/mylite-storage/src/storage.c::load_cached_exact_index_entry_in_statement()`
  returns without creating a cache when the mutation statement chain has a
  deferred durable-cache retarget for the table.
- `packages/mylite-storage/src/storage.c::seed_active_exact_index_cache()`
  already contains the narrower safety check: it refuses to seed from a durable
  exact-index cache while a deferred durable-cache retarget is pending.
- `packages/mylite-storage/src/storage.c::load_complete_exact_index_cache()`
  can populate an active cache from the current checkpoint view by reading
  published leaf entries plus append-tail entries, or the live append history
  fallback.
- Row insert, update, and delete paths already call active exact-index cache
  maintenance hooks before top-level commit promotion.

## Design

- Allow `load_cached_exact_index_entry_in_statement()` to create an active
  exact-index cache even when the mutation statement chain has a deferred
  durable-cache retarget.
- Keep `seed_active_exact_index_cache()` conservative: a pending durable-cache
  retarget still prevents copying a possibly stale durable cache into the
  active statement.
- When durable seeding is skipped, populate the active cache from the current
  checkpoint's live view through `load_complete_exact_index_cache()`.
- Leave durable-cache promotion and retargeting unchanged. Top-level commit
  still applies the deferred durable-cache retarget before promoting maintained
  active exact-index caches.

This keeps the safety boundary where it matters: stale durable caches are not
trusted while mutation state is pending, but active statement-local caches can
be built from the current transaction view and maintained by existing mutation
hooks.

## Affected Subsystems

- MyLite storage exact-index lookup.
- Active storage checkpoint cache creation and maintenance.
- Prepared direct-update performance over routed MyLite tables.

No SQL parser, catalog metadata, public API, or handler admission behavior is
changed.

## Compatibility Impact

No SQL-visible behavior change is intended. Exact-key lookups still return the
current active checkpoint view, including uncommitted changes in the same
transaction and rolled-back changes after savepoint rollback. The change only
alters whether that view is repeatedly scanned or cached inside the active
statement.

## Single-File And Embedded Lifecycle Impact

No file-format, sidecar, lock, recovery, or embedded open/close lifecycle
change. Active exact-index caches remain process-local transient state and are
discarded on rollback or invalidation.

## Public API And File-Format Impact

No public `libmylite`, MyLite storage C API, or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small storage-control-flow change only; no dependency or embedded profile
change is expected.

## Tests And Verification

- `git diff --check` passes.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passes.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passes; `libmariadbd.a` remains 20.21 MiB.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  passes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test`
  passes.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage.capabilities' --output-on-failure`
  passes, including
  `test_active_exact_index_cache_after_mutation_creation()`.
- `ctest --test-dir build/storage-smoke-dev -R 'libmylite.embedded-storage-engine' --output-on-failure`
  passes.
- `ctest --preset storage-smoke-dev --output-on-failure` passes.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 10000`
  completes with:
  - bind: 0.030 us/op;
  - execute: 482.426 us/op;
  - reset: 0.028 us/op.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  completes with execute at 49.498 us/op on one run and 52.187 us/op on a
  delayed-sample run.
- The previous local 1000-row/10000-iteration execute datapoint was
  4387.370 us/op, so the same local benchmark shape improves by about 9.1x.
- One-second samples of the 100000-iteration component run show the first
  expensive cache creation in `load_complete_exact_index_cache()` /
  `read_live_index_entries_from()`. After that one-time active cache build,
  the run amortizes to about 50 us/update. The next bottleneck is avoiding a
  complete live-index cache build for sparse prepared-update working sets.

## Acceptance Criteria

- Exact-key reads after a prior mutation in the same explicit transaction can
  create and reuse an active exact-index cache.
- Pending durable-cache retargets still prevent durable exact-index cache
  seeding.
- Successful insert, update, and delete mutations keep active exact-index
  caches correct until commit.
- Statement rollback and catalog/truncate invalidation continue to discard
  uncertain exact-index caches.
- Prepared primary-key update execute time improves materially on the local
  component benchmark without breaking storage smoke coverage.

## Risks And Unresolved Questions

- Building a complete active exact-index cache after mutation may pay an
  up-front scan cost on the first post-mutation lookup. This is acceptable for
  repeated prepared updates, but future work should add a bounded partial
  active cache for sparse working sets.
- Broader cache retention across failed multi-step statements is not expanded
  here. Existing rollback and invalidation paths remain the guardrail.
