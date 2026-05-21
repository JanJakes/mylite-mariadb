# Existing Exact Cache Hot Probe

## Roadmap Slice

- Row and index storage
- SQL execution API performance
- Spec slug: `existing-exact-cache-hot-probe`

## Source Authority

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Relevant MyLite storage path:
  - `packages/mylite-storage/src/storage.c`

## Problem

Prepared primary-key updates repeatedly probe the same active exact-index cache
after the first execution has created and seeded it. The current helper keeps
the cache-hit path in the same function as the cache-miss path that may create
a cache, seed it from durable state, or load append-history entries. Sampling
still shows that wrapper in the hot prepared-update stack.

## Design

Split the existing-cache probe from the cold cache population path:

- first try a small hot-inline helper that only looks for an existing
  statement exact-index cache and probes it;
- if that helper misses, fall back to the existing creation, seeding, and
  append-history loading path;
- preserve deferred durable-cache retarget checks before creating a new active
  cache.

The hot helper does not change cache contents. It only avoids running through
the miss-handling wrapper once the statement cache is already present.

## Compatibility Impact

No SQL-visible behavior changes. Exact-index caches remain transient
accelerators over the same active checkpoint and durable file visibility rules.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public API or file-format change.

## Test Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run `mylite_storage_test`.
- Run the focused storage and embedded storage-engine CTest subset.
- Run `git diff --check`.
- Run `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`.
- Run a local prepared-update performance baseline.

## Acceptance Criteria

- Existing active exact-index cache hits use the hot-inline probe path.
- Cache misses still create, seed, or load active exact-index caches exactly as
  before.
- Deferred durable-cache retargeting still suppresses new active cache creation.
- Focused storage and embedded storage-engine tests pass.

## Risks

- The split must not bypass miss-time cache seeding from durable caches. The
  existing-cache helper therefore returns "unused" on a miss and lets the cold
  path keep the previous behavior.
