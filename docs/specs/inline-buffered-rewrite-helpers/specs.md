# Inline Buffered Rewrite Helpers

## Problem

After coalescing duplicate statement-buffer lookups, the prepared-update profile
shows `rewrite_buffered_row_page()` and `rewrite_buffered_index_entry_page()` as
hot MyLite-owned leaf functions. These helpers rewrite already-buffered row and
index-entry pages during in-transaction update fast paths.

The helpers are small and private. Most work is fixed header updates plus a
payload/key copy, so the remaining function-call boundary is profile-visible
without adding useful abstraction at the hot call site.

## Source Findings

- MariaDB base line: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite-owned source: `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` calls both helpers after validating that the
  buffered row and index-entry pages belong to the same row shape.
- `rewrite_buffered_row_page()` rewrites row record size, clears the overflow
  root, copies row payload bytes, and zeroes any old tail.
- `rewrite_buffered_index_entry_page()` rewrites index number and key size,
  copies key bytes, and zeroes any old key tail.
- The latest prepared-update sample lists both helpers as hot top-of-stack
  symbols after prior accessor and statement-lookup slices.

## Design

Use the existing private `MYLITE_STORAGE_HOT_INLINE` macro for:

- `rewrite_buffered_row_page()`
- `rewrite_buffered_index_entry_page()`

Do not change their byte-level behavior, validation responsibility, page layout,
or checksum-dirty handling. Checksums remain marked by the caller.

## Compatibility Impact

No SQL-visible behavior change.

## File And API Impact

No public API, file-format, or companion-file change.

## Storage Routing Impact

No routing change.

## Binary-Size Impact

Expected negligible. The helpers have two call sites in one translation unit and
contain small fixed page-field updates.

## Test And Verification Plan

- Build first-party storage smoke targets.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run prepared-update benchmark repeats.
- Sample a prepared-update run and check whether the two rewrite helper symbols
  disappear from the searched top-stack markers.

## Acceptance Criteria

- Storage-smoke coverage remains green.
- The helper bodies preserve the current row/index page bytes.
- Prepared-update timing does not regress materially.

## Risks

- Inlining must not invite broader row-format rewrites. This slice is only about
  removing the helper call boundary.

## Verification

Environment: AppleClang 21.0.0.21000101 on macOS, storage-smoke preset, MariaDB
embedded archive built with `-DPLUGIN_MYLITE_SE=STATIC`.

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_embedded_storage_engine_test`: passed.
- `git diff --check`: passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: passed, 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000`: prepared primary-key update repeats were 4.034, 4.179, and
  4.083 us/op.
- `sample` over a prepared-update run no longer found
  `rewrite_buffered_row_page()` or `rewrite_buffered_index_entry_page()` in the
  searched top-stack markers.
