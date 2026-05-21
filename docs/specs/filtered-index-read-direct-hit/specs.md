# Filtered Index Read Direct Hit

## Roadmap Slice

- Row and index storage
- Spec slug: `filtered-index-read-direct-hit`

## Source Authority

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Relevant upstream-derived handler boundary:
  - `mariadb/storage/mylite/ha_mylite.cc`
  - `handler::index_read_map()` / `handler::index_read_idx_map()` call into
    the engine with a key tuple and an `enum ha_rkey_function`.

## Problem

MyLite's handler builds filtered cursors for `HA_READ_KEY_EXACT` and
`HA_READ_PREFIX` requests. For those requests, `build_index_cursor()` has
already restricted the cursor to matching entries by raw exact lookup or by
MariaDB key-tuple comparison.

After building that filtered cursor, `index_read_map()` and
`index_read_idx_map()` still call `mylite_find_index_entry()`. On hot prepared
point updates this repeats key comparison work against a cursor that is already
known to contain only matches.

## Design

When the handler request uses a positive-length exact or prefix filter:

- build the filtered cursor as before;
- if the filtered cursor is empty, return `HA_ERR_KEY_NOT_FOUND`;
- otherwise read cursor entry `0` directly.

All other read modes keep the existing cursor search because range, before-key,
after-key, and prefix-last semantics require locating a boundary inside an
ordered cursor.

## Compatibility Impact

No SQL-visible behavior changes. The storage layer and file format are
unchanged. The change only removes redundant handler-side lookup work after the
cursor builder has already applied the same exact or prefix predicate.

## File Lifecycle Impact

None. The change does not alter MyLite-owned durable files, journals, locks, or
temporary companions.

## Test Plan

- Rebuild the MyLite storage-smoke MariaDB archive with
  `-DPLUGIN_MYLITE_SE=STATIC`.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run `mylite_storage_test`.
- Run the storage and embedded storage-engine CTest subset.
- Run `git diff --check`.
- Run `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`.
- Run a local prepared-update performance baseline as a sanity check.

## Acceptance Criteria

- Exact and prefix handler reads continue to return matching rows and
  end-of-file/key-not-found conditions through existing coverage.
- The storage-smoke performance baseline still completes successfully.
- No durable storage format, catalog, or recovery behavior changes.

## Risks

- This fast path must stay limited to filtered `HA_READ_KEY_EXACT` and
  `HA_READ_PREFIX` requests. Other `ha_rkey_function` modes must keep the
  ordered cursor boundary search.
