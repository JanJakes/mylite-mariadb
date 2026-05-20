# Inline Storage Page Accessors

## Problem

The prepared-update profile still shows `get_u32_le()`, `put_u32_le()`, and
`put_u64_le()` as hot MyLite-owned leaf functions. These helpers read and write
fixed-width little-endian fields in MyLite pages, and the call overhead is
visible because row-DML touches page headers, row payload headers, index-entry
headers, and buffered checkpoint metadata repeatedly.

## Source Findings

- MyLite-owned storage source:
  `packages/mylite-storage/src/storage.c`.
- The helpers are private static functions declared near the top of the file
  and defined near the end.
- They have no side effects beyond reading or writing the supplied byte buffer.
- The current profile after lazy nested checkpoint header materialization still
  lists `get_u32_le()`, `put_u32_le()`, and `put_u64_le()` as hot symbols.

## Design

Make the small little-endian page accessors use a private
`MYLITE_STORAGE_HOT_INLINE` macro:

- `get_u32_le()`
- `get_u64_le()`
- `put_u32_le()`
- `put_u64_le()`
- `advance_checksum_zero_bytes()`

Keep the existing byte-wise implementation to preserve unaligned-access safety
and host-endianness independence. Do not introduce type-punning or platform
byte-order assumptions.

Plain `static inline` was not enough with the current AppleClang storage-smoke
build: sampling still showed the accessor helpers as leaf functions. The macro
therefore expands to `static inline __attribute__((always_inline))` on Clang and
GCC, and to `static inline` elsewhere.

## Compatibility Impact

No SQL-visible behavior change.

## File And API Impact

No public API, file-format, or companion-file change.

## Storage Routing Impact

No routing change.

## Binary-Size Impact

Expected negligible change. The helper bodies are tiny and already used many
times in one translation unit.

## Test And Verification Plan

- Build first-party storage smoke targets.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample the benchmark and check whether the accessor helper symbols disappear
  or drop materially from the top-of-stack profile.

## Acceptance Criteria

- Storage-smoke coverage remains green.
- The accessor helpers stay byte-wise and portable.
- Prepared-update timing does not regress.

## Risks

- Over-aggressive accessor rewrites could break unaligned or cross-endian file
  behavior. This slice intentionally changes only inlining, not encoding.

## Verification

Environment: AppleClang 21.0.0.21000101 on macOS, storage-smoke preset, MariaDB
embedded archive built with `-DPLUGIN_MYLITE_SE=STATIC`.

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all
  -DPLUGIN_MYLITE_SE=STATIC`: passed; produced `libmariadbd.a` at 20.08 MiB.
- `cmake --preset storage-smoke-dev`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_embedded_storage_engine_test`: passed.
- `cmake --build --preset storage-smoke-dev`: passed after the fresh build tree
  needed all ctest executables.
- `git diff --check`: passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: passed, 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000`: prepared primary-key update repeats were 4.233, 4.449, and
  4.399 us/op.
- `sample` over a prepared-update run no longer found `get_u32_le()`,
  `get_u64_le()`, `put_u32_le()`, `put_u64_le()`, or
  `advance_checksum_zero_bytes()` in the searched stack markers.
