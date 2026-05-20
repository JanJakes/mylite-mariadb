# Row ID Cache Hash Fast Path

## Problem Statement

Prepared primary-key updates still show row-id cache lookup cost in active
rewrite shape and row-payload cache paths. These caches use `hash_row_id()` for
transient in-memory bucket placement, and the current finalizer performs two
64-bit multiplications plus several shifts for every probe.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns row-id cache hashing in
  `packages/mylite-storage/src/storage.c`.
- `hash_row_id()` feeds transient buckets for row-payload caches, active update
  rewrite-shape caches, and exact-index row-id invalidation buckets.
- Bucket lookups still compare the stored row id after hashing, so hash output
  affects performance distribution but not correctness.

## Proposed Design

- Replace the two-multiply finalizer with one odd 64-bit multiplication and a
  high-bit fold into low bucket bits.
- Keep row-id equality checks unchanged in every bucket lookup.
- Keep this limited to transient process-local caches; no durable bytes or SQL
  semantics depend on the hash value.

## Affected Subsystems

- Active row-payload cache lookup.
- Active update rewrite-shape cache lookup.
- Exact-index row-id invalidation cache buckets.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. Cache bucket placement changes only within the current process.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

No new dependency.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Row-id cache lookups use the lighter hash without changing row-id equality.
- Storage and embedded tests pass.
- Prepared-update profiling reduces row-id cache lookup/hash samples or shows
  the next bottleneck clearly.

## Risks And Open Questions

- A cheaper hash can increase collisions for some row-id patterns. Existing
  tests cover cache correctness, while perf sampling must confirm the common
  prepared-update pattern remains healthy.
