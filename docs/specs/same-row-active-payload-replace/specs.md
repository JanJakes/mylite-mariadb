# Same-Row Active Payload Replace

## Problem

Prepared primary-key updates now commonly rewrite the active buffered row page
in place, so the logical current row id does not change. The active row-payload
cache maintenance path still first probes the row-payload bucket to decide
whether the old row is cached, then calls a replacement helper that probes the
same bucket again. The current sampled prepared-update profile shows
`replace_active_row_payload_in_cache()`,
`replace_active_row_payload_cache_entry()`, and row-payload bucket lookup under
the row-update hot path.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `update_row_with_index_entries()` calls `replace_active_row_payload_in_cache()`
  after a successful row update.
- Active buffered row rewrites set `position.row_page_id` to the source row id,
  so the common prepared-update benchmark calls active payload replacement with
  `old_row_id == new_row_id`.
- `replace_active_row_payload_in_cache()` probes the cache with
  `find_row_payload_cache_entry()`.
- `replace_active_row_payload_cache_entry()` then probes the mutable bucket for
  the same old row id before updating the cached row bytes.

## Design

- Make `replace_active_row_payload_in_cache()` resolve the mutable bucket once.
- For `old_row_id == new_row_id`, update the cached entry directly:
  - preserve the existing active cache byte-budget checks;
  - reuse the row allocation when the replacement row has the same size;
  - allocate a new row buffer only when the row size changes;
  - leave the bucket mapping untouched;
  - keep active checksums unset because active readers trust cache ownership.
- Keep the existing `replace_active_row_payload_cache_entry()` path for row-id
  changing replacements.
- Keep stale-entry removal on allocation failure, byte-budget failure, or
  unsupported zero replacement row id.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or file-format behavior changes.
The cache remains a best-effort transient active-checkpoint optimization, and
miss/failure behavior still falls back by removing the cached row.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The slice only changes process-local active row-payload cache maintenance after
successful updates.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 1000 1000000`
  - sampled prepared-update run to confirm duplicate active row-payload bucket
    replacement work moves down
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Same-row active payload replacement performs one bucket lookup and updates the
  cached entry directly.
- Row-id-changing replacement keeps the existing bucket maintenance behavior.
- Active cache byte limits and stale-entry removal behavior remain unchanged.
- Focused storage and embedded storage-engine tests pass.

## Risks

- A failed same-row allocation must not leave stale cached payload bytes. The
  implementation removes the cached row on allocation or byte-budget failure,
  preserving best-effort cache semantics.
