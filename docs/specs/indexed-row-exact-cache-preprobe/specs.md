# Indexed Row Exact Cache Preprobe

## Problem

The active exact-index cache hot path is split from cache population, but the
prepared row-only update profile still enters the larger
`find_exact_index_row_id()` helper on every indexed-row payload lookup. That
helper has to own the cold cache creation, durable seeding, leaf, and append
history paths, even though the steady prepared-update loop usually only needs
the already-populated active exact-index cache.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `ha_mylite::direct_update_rows()` reads the candidate row through
  `ha_mylite::read_exact_unique_index_row_into()`.
- `read_exact_unique_index_row_into()` uses
  `mylite_storage_find_indexed_row_in_statement_into()` when an active storage
  checkpoint is available.
- `find_indexed_row_payload_with_header()` already has the resolved
  `table_entry` and active cache statement before it asks
  `find_exact_index_row_id()` for the matching row id.
- `find_existing_cached_exact_index_entry_in_statement()` is a hot inline probe
  that has the exact active-cache semantics needed for this caller.

## Design

In `find_indexed_row_payload_with_header()`:

- probe the existing active exact-index cache immediately after table-entry
  resolution,
- if that probe returns an active cache result, use it directly,
- if no active cache was usable, fall back to `find_exact_index_row_id()` with
  the same arguments as before.

This keeps all cache-miss behavior in the existing helper while avoiding the
large miss-capable function frame on steady active-cache hits.

## Affected Subsystems

- MyLite storage indexed-row payload lookup.
- Prepared direct update storage hot path.

## Compatibility Impact

No SQL, C API, storage-engine routing, DDL metadata, or file-format behavior
changes. The preprobe calls the same existing active-cache helper and preserves
the same no-fallback semantics when an active exact cache exists but does not
contain the requested key.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, sidecar, or recovery change. The change only
reorders transient cache lookup work inside an already-open statement view.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Binary-Size, License, And Dependency Impact

Small first-party storage branch only. No dependency or license change.

## Test And Verification Plan

- Rely on existing exact-index, indexed-row lookup, active-cache mutation, and
  storage-smoke tests for correctness across cache hits, cache misses,
  duplicate keys, replacement, deletion, and rollback.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage tests and full storage-smoke CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run the focused prepared row-only update component benchmark and sample as
  noisy local regression evidence.

## Completed Verification

On 2026-05-23:

- `git diff --check` passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c` passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test` passed,
  including non-unique exact-key first-match coverage.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=10000000
  10000` reported `1.291 us/op` for the prepared row-only update step
  component in the unsampled run.
- A 2-second sample of the same phase written to
  `/tmp/mylite-indexed-row-exact-cache-preprobe.sample.txt` no longer showed
  `find_exact_index_row_id()` in the hot indexed-row payload lookup path.

## Acceptance Criteria

- Indexed-row payload lookup can return from the active exact-index cache
  without entering `find_exact_index_row_id()`.
- Active exact-cache misses still enter the existing cache population and
  storage lookup path.
- Existing exact-index cache semantics, including duplicate-key first-match
  behavior, remain unchanged.
- Existing storage and embedded storage-engine tests pass.
- Prepared row-only update profiling shows lower visibility for
  `find_exact_index_row_id()` or neutral timing without a correctness
  regression.

## Risks

- The caller must preserve the existing meaning of `used_cache`: an existing
  active exact cache is authoritative even when the requested key is absent.
- A miss should not skip durable seeding or append-history loading; it therefore
  falls through to the unchanged helper.
