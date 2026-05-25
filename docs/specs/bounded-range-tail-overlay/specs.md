# Bounded Range Tail Overlay

## Problem

Bounded forward range cursor batches currently work only for static no-tail
published leaf and branch roots. As soon as later row-state or index-entry
pages exist after the root, storage returns `MYLITE_STORAGE_UNSUPPORTED` and
the handler falls back to full suffix materialization. That keeps correctness,
but it loses the range `LIMIT` win for common tables that have a rebuilt index
root plus a small append tail.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::read_range_first()` starts range access
  with `ha_index_read_map()`, and
  `handler::read_range_next()` advances non-equality ranges with
  `ha_index_next()` before MariaDB checks the end range.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::build_index_cursor()`
  now tries `mylite_storage_read_limited_index_entries_from_prefix()` for
  durable raw forward lower-bound cursors and falls back to full
  `mylite_storage_read_index_entries_from_prefix()` only when storage reports
  `MYLITE_STORAGE_UNSUPPORTED`.
- `ha_mylite::continue_index_cursor()` resumes bounded batches from the last
  emitted `(key, row_id)` pair. The storage API must preserve that exclusive
  resume contract when append-tail entries are present.
- `packages/mylite-storage/src/storage.c::read_index_leaf_entries_from_prefix()`
  already combines a published root suffix with later append-tail visibility by
  calling `read_live_index_entries_from()` after the static leaf/branch read.
- `read_live_index_entries_from()` scans pages after the root, appends matching
  index-entry pages, and applies row-state tombstones/replacements to entries
  already present in the output entryset.
- The current bounded helper rejects any later row-state or matching
  index-entry page through `index_tail_overlay_may_affect_entries()` because a
  tail entry can sort before later static entries and a tail row-state page can
  hide entries in the first static batch.

## Scope

- Extend bounded durable raw forward range reads to roots with append-tail
  overlays.
- Keep the tail scan eager and complete for this slice, while bounding static
  root entry materialization.
- Apply row-state visibility to the bounded static candidate set and all tail
  index entries before returning ordered entries.
- Continue to reject unsupported root shapes or corrupt ordering rather than
  changing SQL-visible results.
- Keep the existing static no-tail path intact.

## Non-Goals

- No persistent merge cursor state in the file format.
- No incremental tail scan or tail index structure.
- No reverse range optimization.
- No support for collation-sensitive, nullable, or BLOB/TEXT key ordering.
- No write-path or compaction change.

## Design

The storage layer will keep using the current bounded static reader to collect
ordered static candidates, but it will no longer reject a root merely because
later overlay pages exist. For roots with a tail:

1. over-read static candidates by the append-tail page span, which is a
   conservative upper bound for tail row-state pages that can hide static
   entries;
2. scan the append tail once with the existing live-entry overlay logic;
3. preserve row-state tombstone/replacement behavior for static candidates;
4. merge/sort static candidates plus live tail entries by raw `(key, row_id)`;
5. discard entries at or below the exclusive resume `(after_key, after_row_id)`;
6. return at most `max_entry_count` entries plus `out_complete`.

Because row-state pages can hide many static candidates, the implementation
must not assume that one static batch is enough. It should fetch static
candidates until either:

- enough live static candidates exist to cover the requested batch after tail
  visibility is applied, or
- the static root suffix is exhausted.

The tail remains eager in this slice. That is a deliberate bounded compromise:
newer append tails are normally smaller than the full published root suffix,
and this preserves correctness without introducing a durable tail index. Using
the tail page span may over-read more static entries than the exact row-state
count, but it avoids a second tail scan and stays bounded by the already-eager
tail work.

## Compatibility Impact

SQL-visible behavior must stay identical. MariaDB still owns range planning,
end-range checks, and row continuation through `read_range_next()`. The new path
is selected only for the same byte-safe raw forward lower-bound cursors already
covered by the bounded static path. Unsupported key shapes keep the existing
full cursor path.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. Reads stay inside the current
primary `.mylite` lifecycle and scoped read statements. Tail overlay pages are
existing durable row-state and index-entry pages.

## Public API And File-Format Impact

No new public API is required. The existing
`mylite_storage_read_limited_index_entries_from_prefix()` contract broadens
from static no-tail roots to overlay roots when storage can prove ordered live
results.

## Storage-Routing Impact

The handler route remains storage-engine-name agnostic: routed `ENGINE=InnoDB`,
`ENGINE=MyISAM`, `ENGINE=Aria`, and explicit MyLite durable tables all use the
same MyLite handler when their cursor and key shape are eligible.

## Wire-Protocol Or Integration-Package Impact

No wire-protocol, client library, or integration-package change.

## Binary-Size, License, And Dependency Impact

First-party storage and test code only. No dependency or license change.

## Test Plan

- Add storage unit coverage for bounded range reads over a published branch root
  with later tail appends that sort before and inside the first static batch.
- Add coverage where tail row-state pages hide static candidates, forcing the
  bounded reader to pull additional static candidates to fill the batch.
- Add continuation coverage across a tail-overlay root.
- Add embedded storage-engine coverage showing a forced secondary range
  `LIMIT 1` query uses the bounded limited API and not the full prefix API after
  appending rows beyond the rebuilt root.
- Run storage-smoke storage and embedded storage-engine tests, whitespace and
  formatting checks.

## Acceptance Criteria

- Append-tail overlay roots can return bounded forward range batches without
  falling back to full suffix materialization.
- Batches remain ordered by raw `(key, row_id)` and honor exclusive resume.
- Tail row-state pages correctly hide replaced or deleted static entries.
- The bounded path does not add a separate tail pre-scan before the live overlay
  scan.
- Existing static no-tail bounded range behavior remains unchanged.
- Docs and roadmap describe that the tail remains eagerly scanned until a later
  tail index or durable merge cursor exists.

## Verification Results

- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|embedded-storage-engine' --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c packages/libmylite/tests/embedded_storage_engine_test.c`

## Risks And Open Questions

- A very long append tail can still dominate runtime because this slice scans
  the complete tail. That is acceptable only as a step toward bounded static
  root reads, not a final SQLite-like cursor design.
- The implementation needs careful completion semantics: returning
  `out_complete=1` is valid only when both the static suffix and overlay input
  are exhausted after resume filtering.
- The raw sort helper is currently used in rebuild paths; reusing or extending
  it for returned entrysets must avoid accidental ownership or allocation churn.
