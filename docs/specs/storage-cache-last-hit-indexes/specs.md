# Storage Cache Last-Hit Indexes

## Problem

Prepared primary-key update profiling still spends visible time repeatedly
scanning small active storage cache sets. The hot path has already resolved the
active statement, table metadata, and current header, but row-DML still revisits
live-row-id, live-row visibility, and row-payload cache arrays for the same
table on each row update.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  resolves the active cache statement once, then repeatedly calls
  `find_table_id_in_statement()`, `live_row_cache_for_statement()`,
  `active_row_payload_cache_for_resolved_statement()`, and
  `seed_active_live_row_id_cache_in_statement()`.
- Active table-entry cache lookup already short-circuits identical schema and
  table string identities before falling back to value comparison.
- `find_active_live_row_id_cache()`, `find_durable_live_row_id_cache()`,
  `find_live_row_cache()`, and row-payload cache lookup still scan from the
  first cache entry each time.
- The relevant cache arrays are statement-local or thread-local transient state;
  they are invalidated by header catalog root/generation, page count where
  durable visibility requires it, filename, and table id checks rather than by
  durable file-format state.

## Design

- Add a tiny last-hit index to the active live-row cache set,
  live-row-id cache set, and row-payload cache set.
- Probe the last-hit entry before the existing linear scan.
- Return a cached entry only after revalidating the same keys the linear scan
  uses today.
- Update the last-hit index when a lookup or append succeeds.
- Clear or naturally invalidate the index when a cache set is cleared, released,
  compacted, or reused.
- Also use filename pointer identity before falling back to `strcmp()` in the
  live-row-id cache lookup, matching the existing row-payload cache pattern.

## Scope

In scope:

- First-party storage cache lookup overhead on hot active row-DML paths.
- Active and durable live-row-id cache lookup.
- Active live-row visibility and active/durable row-payload cache lookup.
- Prepared update performance evidence.

Out of scope:

- B-tree or pager navigation work.
- Cache capacity policy changes.
- Row visibility, transaction, savepoint, rollback, or recovery semantics.
- Public API, SQL behavior, storage routing, or file-format changes.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or MariaDB compatibility behavior
changes. The optimized lookup returns only entries that pass the same
file/header/table validation as the existing scans.

## Single-File And Lifecycle Impact

No durable storage layout or companion-file lifecycle change. The last-hit
indexes are transient in-memory hints on statement-local and thread-local cache
sets.

## Test Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.
- Sample the prepared update phase and confirm the new fast paths do not expose
  correctness regressions; treat wall-clock changes as noise unless repeated
  runs show a clear movement.

## Acceptance Criteria

- Existing storage and embedded routed-engine tests pass.
- Cache fast paths revalidate filename, catalog root/generation, page count
  where applicable, and table id before returning.
- Cache clear, compaction, append, and reusable-cache paths cannot return a
  stale entry.
- Prepared update profiling remains correct, with larger remaining costs
  attributed to active page rewrite/undo and MariaDB quick-range execution
  rather than this transient cache bookkeeping.

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
  `2.469 us/op`; the sampled run measured `2.550 us/op`.
- A two-second macOS `sample` run over the same phase showed the next dominant
  storage-side work in active update page rewrite, buffered undo capture,
  table-id lookup, and durable live-row-id seeding. MariaDB quick-range
  construction and execution also remain visible.
