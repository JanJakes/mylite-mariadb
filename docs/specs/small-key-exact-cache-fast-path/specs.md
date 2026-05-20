# Small Key Exact Cache Fast Path

## Problem Statement

Prepared primary-key updates still spend samples in exact-index cache lookup,
including `memcmp()` equality checks on small fixed-width keys. The performance
baseline uses `INT NOT NULL PRIMARY KEY`, so its routed point updates repeatedly
probe exact-index caches with small key images.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns exact-index cache lookup in
  `packages/mylite-storage/src/storage.c`.
- `find_exact_index_cache_entry_row_id()` and
  `append_exact_index_cache_matches_to_entryset()` use `memcmp()` for equality
  even when the key image is 1, 2, 4, or 8 bytes.

## Proposed Design

- Add a small-key equality helper for exact-index cache probes.
- Compare 1, 2, 4, and 8 byte key images with fixed-size loads.
- Keep existing `memcmp()` fallback for all other key sizes.
- Leave exact-index hash distribution unchanged until profiling shows a clear
  net win from changing it.

## Affected Subsystems

- Exact-index cache lookup and exact-entryset lookup.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, hash distribution, or
file-format behavior changes. Equality remains byte-exact.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

Small first-party helper split with no new dependency.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Exact-index cache equality for 1/2/4/8 byte keys avoids libc compare
  overhead.
- Existing variable-width and larger key behavior remains unchanged.
- Storage and embedded tests pass.
- Prepared-update profiling reduces exact-index key compare samples or shows
  the next bottleneck clearly.

## Risks And Open Questions

- Extra helper branches could cost more than `memcmp()` on some compilers, so
  perf evidence must stay visible in the slice notes.
