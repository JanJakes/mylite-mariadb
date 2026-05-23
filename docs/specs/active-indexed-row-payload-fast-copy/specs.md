# Active Indexed-Row Payload Fast Copy

## Problem

After splitting the uncached indexed-row materialization fallback, sampled
`prepared-row-only-update-components` profiles still show
`read_indexed_row_payload_from_open_file()` on the hot exact-row materialization
path. In the repeated prepared update benchmark, `find_indexed_row_payload_with_header()`
already has the active statement cache, table id, and resolved row id before it
calls the generic materializer. For active row-payload cache hits, the generic
materializer only repeats the active cache probe and copies the cached row into
the caller's output buffer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `find_indexed_row_payload_with_header()` resolves table metadata, exact index
  row id, and active row-payload cache ownership before row materialization.
- `read_indexed_row_payload_from_open_file()` first probes the same active
  row-payload cache and copies the cached row when it is usable.
- Existing active payload cache hits are also treated as live-row validation
  proof by suppressing the later `mark_active_validated_live_row_in_statement()`
  call through the `active_payload_cached` flag.

## Design

In `find_indexed_row_payload_with_header()`, probe the already-resolved active
row-payload cache after the exact row id is known. When the entry is usable,
copy it directly with `copy_cached_row_payload()` and set
`active_payload_cached`.

Fall back to `read_indexed_row_payload_from_open_file()` for cache misses,
unusable entries, durable-cache-only reads, and row-page reads. Preserve the
existing not-found-to-corrupt mapping and live-row validation behavior.

## Compatibility Impact

No SQL, public C API, handler API, storage-engine routing, metadata, or
file-format behavior change. The same cached bytes are copied to the same
output target with the same ownership and capacity rules.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.
The change only shortens an active in-memory cache-hit path.

## Public API And File-Format Impact

No public API, internal storage API, or `.mylite` format change.

## Binary-Size And Dependency Impact

Small first-party branch in an existing helper. No dependency change. Binary
size impact should be trivial.

## Tests And Verification

Verified on 2026-05-23 on macOS 26.5 with:

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components --profile-iterations=10000000 10000`
- focused sampled run of the same phase.

The first unsampled run measured prepared row-only update step at
`1.300 us/op`, which was above the current noise band. A sampled run and a
second unsampled run both measured `1.264 us/op`. The sampled run was written
to `/tmp/mylite-active-indexed-row-payload-fast-copy.sample.txt`; it no longer
showed `read_indexed_row_payload_from_open_file()` on the active cache-hit path.
The cached copy work moved into `find_indexed_row_payload_with_header()`.

## Acceptance Criteria

- Active row-payload cache hits after exact index lookup avoid
  `read_indexed_row_payload_from_open_file()`.
- Cache misses and durable-cache/non-statement reads keep the generic
  materialization path.
- Prepared row-only update timing does not regress.

## Risks And Unresolved Questions

- This duplicates one active-cache probe on cache misses because the generic
  materializer still owns the miss and durable-cache fallback paths. That cost
  is not on the steady cache-hit path this slice targets.
