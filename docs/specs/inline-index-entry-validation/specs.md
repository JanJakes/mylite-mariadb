# Inline Index Entry Validation

## Problem

Prepared indexed-update profiling still samples `validate_index_entries()` in
the storage update path. The helper is a small first-party guard over
`mylite_storage_index_entry` shape and key-size limits, but it remains an
out-of-line call on every indexed insert and update.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `packages/mylite-storage/src/storage.c` calls `validate_index_entries()`
  before row insert/update paths that receive index-entry arrays.
- The helper does not allocate, mutate state, perform I/O, or depend on cold
  storage structures.
- MyLite already uses `MYLITE_STORAGE_HOT_INLINE` for small hot storage guards
  and byte helpers in this file.

## Design

- Mark `validate_index_entries()` as `MYLITE_STORAGE_HOT_INLINE`.
- Keep the same misuse and unsupported-result behavior for all callers.
- Do not add trusted API variants; public-ish storage entry points should keep
  validating caller-provided structures.

## Scope

In scope:

- Storage insert/update index-entry validation call overhead.
- Prepared indexed-update performance evidence.

Out of scope:

- Skipping validation.
- Changing handler/storage API contracts.

## Compatibility Impact

No SQL, C API, storage-engine routing, or file-format behavior changes.

## Single-File And Lifecycle Impact

No durable lifecycle change.

## Test Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- Existing storage validation and embedded routed-storage tests pass.
- `validate_index_entries()` no longer appears as an out-of-line sampled frame
  in the prepared indexed-update benchmark.
