# Active Live Row Cache Buckets

## Problem

Prepared primary-key updates in one active transaction repeatedly validate and
retarget rows that are already known to be live in the active checkpoint view.
The current statement live-row cache stores live and payload-validated row ids
in linear arrays. A fresh local sample on 2026-05-20 shows hot time in
`add_live_row_id()`, `add_validated_live_row_id()`,
`active_validated_live_row_known_in_cache()`, and related cache lookup paths
inside prepared point updates.

The arrays are small enough for correctness, but a 1,000-row update loop turns
membership checks into repeated linear scans. This is first-party storage code,
so it is safer to optimize here than in MariaDB optimizer cost hooks.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`) for the embedded handler and
  statement execution path.
- `packages/mylite-storage/src/storage.c` owns the active live-row cache:
  `mylite_storage_live_row_cache`, `add_live_row_id()`,
  `add_validated_live_row_id()`, `remove_live_row_id()`,
  `remove_validated_live_row_id()`, and
  `active_validated_live_row_known_in_cache()`.
- The same file already uses hash-backed row-id structures for row-state maps,
  row-payload caches, and exact-index cache row-id maintenance. Those local
  patterns provide the implementation model.
- The clean local prepared-update baseline before this slice was
  `3.047 us/op` for
  `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.

## Design

- Replace each live-row cache's raw live and validated arrays with two small
  row-id set objects that keep:
  - the existing row-id array for compact storage and predictable cleanup,
  - bucket heads and per-entry next links for larger membership and index
    lookup,
  - swap-removal bookkeeping so update/delete churn does not leave tombstones
    behind.
- Keep small sets linear until at least 32 row ids. This preserves the cheap
  reusable one-row statement-cache path and avoids hash-table reset overhead
  for ordinary nested statement cleanup.
- Keep public and internal semantics unchanged:
  - adding an existing row id is idempotent,
  - removing a missing row id is a no-op,
  - replacement removes old live and validated ids and adds the replacement id,
  - reusable cache cleanup keeps small allocations but clears logical contents.
- Preserve the existing row-id hash helper and chain-bucket style used by the
  adjacent exact-index caches.

## Affected Subsystems

- First-party MyLite storage active-statement live-row cache only.
- No MariaDB optimizer, handler API, SQL result, catalog, file-format, or
  public `libmylite` API behavior changes.

## Compatibility Impact

No SQL-visible behavior changes. The slice only changes the lookup structure
used to remember rows already proven live or payload-validated within the active
statement chain.

## Single-File And Lifecycle Impact

None. The buckets are transient in-memory state and are cleared with the owning
statement cache. Durable `.mylite` bytes, journals, rollback behavior, and
companion-file lifecycle are unchanged.

## Storage-Engine Routing Impact

No routing changes. The optimization applies after a MyLite-routed table is
already participating in active row DML.

## Binary-Size Impact

Negligible. The change adds small first-party helper code and no dependency.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Live and validated row-id cache membership is hash-backed.
- Small live-row id sets stay on the existing linear path until the bucket
  threshold is reached.
- Existing storage and embedded storage-engine tests pass.
- Prepared primary-key update benchmark completes with the expected checksum.
- No file-format, catalog, SQL, or public API behavior changes.

## Verification Results

- `git diff --check`
- `git clang-format --diff`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 tests passed.
- `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`:
  prepared primary-key updates measured `3.073 us/op`, checksum `1512893`.
- Controlled 10,000-row comparison on the same worktree:
  - with `MYLITE_STORAGE_LIVE_ROW_ID_BUCKET_MIN_COUNT` temporarily raised so
    this cache stayed linear,
    `tools/mylite-perf-baseline --phase=prepared-updates 10000 100000`
    measured `28.849 us/op`;
  - with the 32-row bucket threshold restored, the same command measured
    `25.997 us/op`, checksum `50238894`.

## Risks And Unresolved Questions

- Bucket links must move with swapped row-array entries so valid rows remain
  reachable after replacement churn.
- Reusable cache reset must clear bucket state as well as array counts.
