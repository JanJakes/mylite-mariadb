# Forward Range Index Entrysets

## Problem

MyLite exact-key and prefix-key handler reads can already build narrowed
storage entrysets for byte-safe integer key images. Forward range starts such
as `HA_READ_KEY_OR_NEXT` and `HA_READ_AFTER_KEY` still build the complete
ordered cursor before the handler binary-searches to the requested starting
key. That leaves common `WHERE indexed_int >= ? ORDER BY indexed_int LIMIT ...`
plans paying whole-index cursor materialization even when a published
branch/leaf root can skip earlier pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` calls
  `ha_index_read_map()` with the range start key and flag, then uses
  `handler::read_range_next()` / `ha_index_next()` to continue the range.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::index_read_map()` and
  `index_read_idx_map()` currently filter only `HA_READ_KEY_EXACT` and
  `HA_READ_PREFIX`; `HA_READ_KEY_OR_NEXT` and `HA_READ_AFTER_KEY` use a full
  cursor followed by `mylite_find_index_entry()`.
- `ha_mylite::build_index_cursor()` can already choose durable exact and
  prefix entryset APIs when a raw byte comparison is safe, then sort and
  materialize the returned entries through the existing cursor path.
- `packages/mylite-storage/src/storage.c::read_index_leaf_run_root()` exposes
  published leaf and branch roots as a leaf-run view, while
  `find_index_leaf_run_prefix_lower_page()` can find the first leaf page whose
  high key can satisfy a byte-prefix lower bound.
- Existing append-tail overlay reads use
  `read_live_index_entries_from()` after a published leaf run, preserving live
  row-state visibility for mutations after the static root.

## Scope

- Add a first-party durable storage API that returns live index entries from a
  byte-prefix lower bound over published leaf/branch roots, with append-tail
  overlay included.
- Route `HA_READ_KEY_OR_NEXT` and `HA_READ_AFTER_KEY` handler cursor builds to
  that API only for byte-safe raw key filters.
- Keep the existing handler binary search as the authoritative final position
  step.
- Add storage and routed storage-engine tests proving correctness and bounded
  static-root reading.

## Non-Goals

- No reverse range optimization for `HA_READ_KEY_OR_PREV` or
  `HA_READ_BEFORE_KEY`.
- No collation-sensitive, nullable-key, BLOB/TEXT-prefix, or partial key-part
  byte shortcut.
- No new on-disk page format, branch split, merge, compaction, or write-path
  change.
- No volatile MEMORY/HEAP lower-bound entryset API in this slice; volatile rows
  keep the existing full-entryset cursor path.

## Design

Add `mylite_storage_read_index_entries_from_prefix()` to
`packages/mylite-storage/include/mylite/storage.h`. The API accepts a serialized
byte prefix and returns the live entries from the first static leaf page that
can contain a key greater than or equal to that prefix through the end of the
published root, plus the append-tail overlay.

The storage implementation will:

- resolve the table and index root through the same active table-entry cache
  used by the existing entryset readers;
- use the existing leaf-run root reader so single-page leaf roots, contiguous
  leaf runs, and multi-level branch roots share one path;
- lower-bound to the first candidate leaf with
  `find_index_leaf_run_prefix_lower_page()`;
- lower-bound within the first leaf page before appending entries, then append
  complete later leaves;
- append any live tail entries from `leaf_run.tail_page_id` with
  `read_live_index_entries_from()`;
- fall back to the full live index entryset when no eligible static root is
  published, preserving correctness for append-only histories.

`ha_mylite::build_index_cursor()` will gain an internal cursor mode:

- full cursor for scans and unsupported filters;
- match cursor for exact/prefix equality filtering;
- forward lower-bound cursor for `HA_READ_KEY_OR_NEXT` and
  `HA_READ_AFTER_KEY`.

Lower-bound cursors are marked filtered because `index_first()` and
`index_last()` must rebuild a complete cursor, but they do not apply equality
or prefix filtering to returned entries. The existing
`mylite_find_index_entry()` binary search still chooses the first `>=` or `>`
entry according to the MariaDB find flag.

## Compatibility Impact

SQL-visible behavior should not change. MariaDB remains responsible for range
planning, end-range checks, and key-tuple comparison. The narrowed durable read
is used only when MyLite already knows raw serialized byte ordering is safe for
the supplied key image. Other key shapes keep the current full cursor and
MariaDB comparison behavior.

## Single-File And Lifecycle Impact

No new companion files or lifecycle states. The slice reads existing
`.mylite` pages under the current scoped read statement and append-tail
visibility rules.

## Public API And File-Format Impact

The only public first-party storage API addition is
`mylite_storage_read_index_entries_from_prefix()`. There is no `libmylite`
application API or file-format change.

## Storage-Routing Impact

The handler route applies to durable MyLite-routed tables, including tables
requested as `ENGINE=InnoDB` and resolved to MyLite. Volatile MEMORY/HEAP
routing remains correct through the existing full cursor path.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol, client library, or integration-package behavior changes.

## Binary-Size, License, And Dependency Impact

Small first-party C/C++ helper code only. No dependency or license change.

## Test Plan

- Add storage unit coverage for `mylite_storage_read_index_entries_from_prefix()`
  over a branch root where an earlier unrelated leaf is checksum-corrupted,
  proving the reader starts at the lower-bound page instead of materializing the
  whole root.
- Cover append-tail overlay entries after the published static root.
- Add routed storage-engine coverage for forced integer-index `>=` and `>`
  reads, including routed `ENGINE=InnoDB`.
- Run storage unit tests, storage-smoke tests, formatting checks, and
  whitespace checks.

## Acceptance Criteria

- Forward durable range cursor starts can avoid reading earlier static leaf
  pages for byte-safe raw key images.
- `HA_READ_KEY_OR_NEXT` and `HA_READ_AFTER_KEY` still return the same rows and
  continue correctly through `index_next()`.
- Equality/prefix cursors, full scans, reverse range starts, and unsupported
  key shapes keep their existing behavior.
- Append-tail mutations remain visible in forward range cursors.

## Verification Results

2026-05-25, macOS arm64 local worktree:

```sh
cmake --build --preset dev --target mylite_storage_test
build/dev/packages/mylite-storage/mylite_storage_test
cmake --build build/mariadb-mylite-storage-smoke --target mylite_se
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
ctest --test-dir build/dev -R mylite-storage --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.h mariadb/storage/mylite/ha_mylite.cc packages/libmylite/tests/embedded_storage_engine_test.c
find /tmp -maxdepth 1 -type d -name 'mylite-storage-test.*' -print
```

All passed; the cleanup probe printed no leftover storage-test directories.

## Risks And Open Questions

- The storage API returns an eager entryset suffix, not an incremental cursor;
  a later B-tree cursor abstraction should replace the remaining suffix
  materialization for long ranges.
- The optimization is intentionally byte-oriented. Extending it to strings or
  nullable keys requires stronger evidence that the serialized key image
  preserves the relevant MariaDB ordering semantics.
