# Bounded Range Tail Filtered Overlay

## Problem

Bounded range reads with append-tail overlays now avoid full static-root suffix
materialization, but the live tail overlay still appends every tail index entry
for the target index before the final lower-bound and resume filter runs. That
keeps correctness, but it creates avoidable entryset growth and sort work when
many appended tail keys sort below the requested range start.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()` uses
  `mylite_storage_read_limited_index_entries_from_prefix()` for eligible raw
  forward range starts and resumes through `continue_index_cursor()`.
- `packages/mylite-storage/src/storage.c::apply_limited_index_tail_overlay()`
  currently calls `read_live_index_entries_from()` and then filters/sorts the
  merged entryset.
- `scan_index_prefix_entries_from()` already shows the required replacement
  ordering pattern for filtered overlays: remove any tracked row id for a tail
  index-entry page first, then append the tail entry only when it matches the
  requested key filter.

## Scope

- Add a bounded-range-specific tail overlay scanner that filters target-index
  tail index entries by raw lower-bound and exclusive resume before appending
  them to the candidate entryset.
- Preserve row-state tombstone and replacement handling for static candidates
  and earlier tail entries.
- Keep the current eager tail page scan and static over-read bound unchanged.

## Non-Goals

- No durable tail index or persistent merge cursor.
- No reverse range optimization.
- No support for nullable, collation-sensitive, or BLOB/TEXT range ordering.
- No benchmark threshold changes.

## Design

The new scanner will follow the same page loop as `read_live_index_entries_from`
but accept the bounded range lower-bound inputs:

1. build tracked row-id state for the static candidate entryset;
2. for every target table/index tail index-entry page:
   - ignore it if a prior row-state page already hides the row id;
   - remove any currently tracked entry for the tail row id, because the tail
     entry supersedes an older key for that live row;
   - append it only if its raw key is at or above `key_prefix` and beyond the
     exclusive `(after_key, after_row_id)` resume point;
3. for every target-table row-state page, keep the existing replace/delete
   behavior against tracked entries.

This preserves the important replacement case where a static key in range is
updated to a new key below range: the row-state page retargets the static entry
to the replacement row id, and the later tail index-entry page removes that
tracked row id even though the replacement key is not appended.

## Compatibility Impact

SQL-visible behavior must stay identical. The scanner changes only which tail
entries are materialized before the final ordered filter. MariaDB still owns
range planning, row ordering requirements, and end-range checks.

## Single-File And Lifecycle Impact

No file-format or companion-file change. Reads still scan existing tail pages
inside the current scoped read lifecycle.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Routing Impact

No routing change. Routed `ENGINE=InnoDB` and explicit MyLite tables share the
same storage path when the handler accepts the raw range shape.

## Binary-Size, License, And Dependency Impact

First-party storage and test code only. No dependency or license change.

## Test Plan

- Add storage unit coverage where a tail replacement moves a static in-range
  entry below the lower bound and must remove the stale static key from the
  bounded result.
- Keep the existing bounded tail overlay ordering and continuation test
  passing.
- Keep the embedded storage-engine range tail regression passing.
- Run focused storage and embedded storage-engine tests plus formatting and
  whitespace checks.

## Acceptance Criteria

- Tail entries below the requested lower-bound/resume point are not appended to
  the bounded result entryset.
- Row-state replacements still remove stale static entries even when the
  replacement key falls below the requested range.
- Existing bounded tail overlay ordering, continuation, and static no-tail
  behavior remain unchanged.

## Verification Results

- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at 21,407,232 bytes.
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|embedded-storage-engine' --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Open Questions

- The tail pages are still scanned eagerly. This reduces materialization and
  sort work, not page-read complexity.
- The scanner must keep replacement ordering precise; filtering before removing
  tracked replacement row ids would reintroduce stale keys.
