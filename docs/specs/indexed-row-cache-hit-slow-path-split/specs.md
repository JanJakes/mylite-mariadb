# Indexed Row Cache-Hit Slow-Path Split

## Problem

The sampled `prepared-row-only-update-components` profile still shows
`read_indexed_row_payload_from_open_file()` as a large MyLite-owned frame.
Disassembly shows the function reserves a `0x13e0` stack frame and calls
`___chkstk_darwin` before it can return the common active row-payload cache hit.
That stack frame exists because the cache-miss path owns a 4096-byte row-page
scratch buffer, even though cache hits only copy an already-retained row payload
into the caller's output buffer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `find_indexed_row_payload_with_header()` calls
  `read_indexed_row_payload_from_open_file()` after resolving the exact row id
  for the selected key.
- `read_indexed_row_payload_from_open_file()` checks active and durable
  row-payload caches before reading a row page from storage.
- The row-page read fallback allocates `row_buffer[MYLITE_STORAGE_FORMAT_PAGE_SIZE]`
  in the same function, so the active-cache hit pays the large stack prologue
  even when no page read can happen.

## Design

Keep `read_indexed_row_payload_from_open_file()` as the cache-hit front end.
Move the row-page read, row copy, active cache store, and durable cache store
fallback into `read_uncached_indexed_row_payload_from_open_file()`.

Mark the uncached helper `MYLITE_STORAGE_NOINLINE` so the compiler keeps the
large page scratch buffer out of the cache-hit frame. Preserve all cache lookup,
invalid durable-entry removal, not-found/error mapping, output buffer, and
active/durable cache population behavior.

## Compatibility Impact

No SQL, public C API, handler API, storage-engine routing, metadata, or
file-format behavior change. The same cache entries and row pages are used with
the same output ownership rules.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.
The change only moves transient stack storage to the cache-miss helper.

## Public API And File-Format Impact

No public API, internal storage API, or `.mylite` format change.

## Binary-Size And Dependency Impact

Adds a small first-party no-inline macro and one split helper. No dependency
change. Binary size may change trivially due to function layout.

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

Disassembly after the split shows `read_indexed_row_payload_from_open_file()`
using a `0x350` frame with no `___chkstk_darwin` call, while the no-inline
`read_uncached_indexed_row_payload_from_open_file()` owns the larger row-page
scratch frame. The sampled run was written to
`/tmp/mylite-indexed-row-cache-hit-slow-path-split.sample.txt`; it did not show
`read_uncached_indexed_row_payload_from_open_file()` or `___chkstk_darwin` under
the active cache-hit row materialization path.

The 10,000,000-iteration unsampled run measured prepared row-only update step
component at `1.271 us/op`. The sampled run measured `1.280 us/op`.

## Acceptance Criteria

- Active row-payload cache hits in `read_indexed_row_payload_from_open_file()`
  no longer reserve the row-page scratch buffer or call `___chkstk_darwin`.
- The uncached row-page fallback remains available and covered by existing
  storage and embedded storage-engine tests.
- Prepared row-only update timing does not regress.

## Risks And Unresolved Questions

- This does not remove the required copy into MariaDB's handler record buffer.
  It only removes cache-hit stack setup and keeps the larger fallback frame off
  the hot path.
