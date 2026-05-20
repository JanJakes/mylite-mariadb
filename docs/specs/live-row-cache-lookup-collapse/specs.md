# Live Row Cache Lookup Collapse

## Problem Statement

Prepared primary-key updates still spend samples in `find_live_row_cache()` after
scoped live-row marking removes statement ownership rediscovery. Validated
live-row marking first calls `mark_active_live_row_in_statement()` and then
looks up the same cache again before appending the validated row id.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns the affected live-row cache helpers in
  `packages/mylite-storage/src/storage.c`.
- `mark_active_live_row_in_statement()` already creates or reuses the
  statement live-row cache for a `(catalog_root_page, catalog_generation,
  table_id)` tuple.
- `mark_active_validated_live_row_in_statement()` currently repeats the same
  cache lookup after the live-row mark succeeds.

## Proposed Design

- Add an internal helper that resolves or appends the live-row cache for a
  statement once and returns the cache pointer.
- Use that helper from both live-row and validated live-row marking.
- For validated marking, add the live row id and validated row id through the
  same cache pointer.

## Affected Subsystems

- Statement-owned live-row validation cache.
- Prepared-update storage read/update hot paths.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. The same row ids are inserted into the same statement-owned caches.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

Small first-party helper with no new dependency.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Validated live-row marking resolves the live-row cache once per mark.
- Existing no-op behavior without an active statement remains unchanged.
- Storage and embedded tests pass.
- Prepared-update profiling reduces duplicate `find_live_row_cache()` samples
  or shows the next bottleneck clearly.

## Risks And Open Questions

- `add_live_row_id()` and `add_validated_live_row_id()` still scan their
  per-cache arrays, so this does not solve all live-row cache cost.
