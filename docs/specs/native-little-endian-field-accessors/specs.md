# Native Little Endian Field Accessors

## Problem Statement

Prepared primary-key update samples still spend time inside
`rewrite_active_update_pages()`, and sampled offsets map to inlined byte loops
from `put_u32_le()` and `put_u64_le()` while rewriting buffered row and index
pages. Those loops preserve little-endian file encoding but are unnecessarily
expensive on the little-endian platforms used for current development and
benchmarking.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns page field accessors in
  `packages/mylite-storage/src/storage.c`.
- MyLite file-format fields are explicitly little-endian and already accessed
  through `get_u32_le()`, `get_u64_le()`, `put_u32_le()`, and `put_u64_le()`.
- The hot buffered rewrite helpers call those accessors for row size,
  overflow root, index number, and key size updates.

## Proposed Design

- Detect native little-endian builds with compiler byte-order macros.
- On native little-endian builds, use `memcpy()` to load 32-bit and 64-bit
  field values and store 32-bit field values. This avoids alignment assumptions
  while letting the compiler use efficient native loads and stores for the
  proven-safe subset.
- Keep 64-bit fast-path locals in the accessor's `unsigned long long` contract
  and assert that the type is exactly 64 bits on native little-endian builds.
- Keep the existing byte-loop implementation as the fallback for other targets
  and for 64-bit stores.

## Affected Subsystems

- Storage page encoding and decoding helpers.
- Active buffered row and index-entry rewrite hot paths.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. Stored bytes remain little-endian; non-little-endian targets keep the
existing byte-loop implementation.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

No new dependency; `string.h` is already used.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

## Acceptance Criteria

- Native little-endian builds avoid per-byte field accessor loops for fixed-width
  reads and 32-bit writes.
- Existing storage tests pass, proving file bytes remain compatible.
- Prepared-update profiling reduces field-accessor samples inside
  `rewrite_active_update_pages()` or reveals the next bottleneck.

## Risks And Open Questions

- The fast path depends on compiler byte-order macros. Targets without those
  macros use the conservative existing byte-loop fallback.
- Native little-endian builds now require 64-bit `unsigned long long`, matching
  the storage API's existing 64-bit field contract.
- Native 64-bit stores are deliberately left on the byte-loop implementation;
  the embedded storage-engine fixture caught corruption when that path used the
  same `memcpy()` treatment.
