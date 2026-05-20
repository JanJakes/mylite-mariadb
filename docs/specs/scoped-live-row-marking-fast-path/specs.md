# Scoped Live Row Marking Fast Path

## Problem Statement

Prepared primary-key updates still show `active_statement_for_file()` samples
after scoped header reads are removed from the hot profile. The remaining
samples come from live-row cache marking in exact-index lookup and indexed-row
payload lookup, even though those paths already opened the file through
`open_existing_file_scope()` and have the matching active write statement.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns the affected paths in
  `packages/mylite-storage/src/storage.c`.
- `open_existing_file_scope()` records the active write statement in
  `mylite_storage_file_scope::active_statement`.
- `mark_active_live_row()` and `mark_active_validated_live_row()` are wrappers
  that rediscover the statement from `FILE *`; their `_in_statement` variants
  already implement the cache update without another ownership lookup.
- The sampled prepared-update run after scoped header reads still showed
  `active_statement_for_file()` under `mark_active_live_row()` and
  `mark_active_validated_live_row()` in indexed-row lookup.

## Proposed Design

- Use `mark_active_live_row_in_statement()` in exact index-entry lookup and
  indexed-row payload lookup after opening a file scope.
- Use `mark_active_validated_live_row_in_statement()` after indexed payload
  validation in the same scoped path.
- Preserve the `FILE *` wrapper helpers for callers that do not carry an active
  file scope yet.

## Affected Subsystems

- First-party MyLite storage indexed read hot paths.
- Active live-row cache marking.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. The cache helpers update the same statement-owned live-row cache that
the generic wrappers would find from the file handle.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

No new dependency. This only changes hot-path helper selection.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Scoped exact-index and indexed-row lookup paths mark live rows through the
  active statement already captured by the file scope.
- Existing callers without scoped statement ownership keep the generic wrappers.
- Storage and embedded tests pass.
- Prepared-update profiling reduces `active_statement_for_file()` samples under
  live-row marking or shows the next bottleneck clearly.

## Risks And Open Questions

- Read-only scopes and snapshot scopes intentionally pass `NULL` to the
  statement-level helpers, matching the generic wrapper behavior when no active
  write statement owns the file.
