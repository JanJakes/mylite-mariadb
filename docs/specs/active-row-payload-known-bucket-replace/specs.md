# Active Row Payload Known Bucket Replace

## Problem

Prepared row-only updates now spend most of their remaining mutable storage cost
inside already-hot active caches. The update path validates the old row through
the active row-payload cache, then row-payload replacement resolves the same old
row id again before retargeting the cached payload to the new row id. For
append-style updates this also removed the old bucket through another hash
lookup after inserting the replacement bucket.

The goal is to remove those redundant old-row bucket probes without changing
row visibility, rollback behavior, SQL semantics, or the `.mylite` file format.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  validates the old row before writing the replacement row and updates active
  statement caches after the mutation succeeds.
- `packages/mylite-storage/src/storage.c::validate_direct_live_row_in_statement_cache()`
  already probes the active row-payload cache before falling back to live-row
  validation or file reads.
- `packages/mylite-storage/src/storage.c::replace_active_row_payload_in_cache()`
  receives the active row-payload entry found by validation, but the slow
  retarget path still re-resolves the mutable bucket for the old row id.
- `packages/mylite-storage/src/storage.c::replace_active_row_payload_cache_entry()`
  inserts the new row-id bucket and used to remove the old row-id bucket through
  `remove_row_payload_cache_bucket()`, which hashes and probes the old row id a
  second time.
- The row-payload cache uses open-addressed buckets with tombstones. Replacing a
  bucket must preserve tombstone accounting and still rebuild when tombstones
  exceed live entries.

## Design

Return the active row-payload cache bucket together with the entry when
`validate_direct_live_row_in_statement_cache()` satisfies validation from the
active row-payload cache.

Thread that bucket through `replace_active_row_payload_in_cache()` and validate
it before use:

- the bucket must be occupied,
- the bucket row id must match the old row id,
- the bucket entry index must still be in range,
- the bucket entry must still identify the same old row id and cache entry.

If any check fails, fall back to the existing hash lookup for the old row id.
Once the replacement bucket is inserted for the new row id, mark the known old
bucket deleted directly and preserve existing tombstone accounting and rebuild
policy.

This keeps the optimization local to a statement-owned active cache. The
generic raw-filename and durable-cache paths keep their existing lookup flow.

## Affected Subsystems

- MyLite storage active row-payload cache validation.
- MyLite storage active row-payload cache replacement after row updates.
- Prepared row-only update storage mutation performance.

## Compatibility Impact

No SQL-visible behavior changes. Existing MariaDB and MyLite behavior for
affected rows, rollback, generated columns, CHECK constraints, FK checks, and
storage-engine routing remains unchanged.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format change is required. The slice only changes in-memory
statement cache bookkeeping and does not introduce durable sidecars or new
embedded runtime state.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No new dependency. The code change is a narrow first-party storage hot-path
optimization.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline -j1`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=storage-row-update-components 10000 1000000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`

Local benchmark evidence after the change:

- storage row update mutation component: `0.288 us/op`
- prepared row-only update step component: `1.593 us/op`

The prior local baseline for the same phases was about `0.307 us/op` and
`1.685 us/op`, respectively. These values are machine-local and should be used
as directional evidence only.

## Acceptance Criteria

- Active row-payload validation can pass the known old-row bucket to replacement.
- Replacement validates the known bucket and falls back to the old lookup when
  the known bucket is unavailable or stale.
- Retargeting a cached payload marks the old bucket deleted without a redundant
  old-row hash probe.
- Existing focused storage and embedded routed-storage tests pass.
- Benchmarks show no regression in row-only update mutation components.

## Risks And Unresolved Questions

- The known bucket is valid only within the current active statement cache
  lifetime. This slice keeps the pointer inside the existing validation to
  replacement sequence and falls back if the bucket no longer matches the old
  row.
- This does not address the larger prepared-update SQL-layer cost in
  `open_tables_for_query()`, `Sql_cmd_update::prepare_inner()`, or
  `JOIN::prepare()`.
