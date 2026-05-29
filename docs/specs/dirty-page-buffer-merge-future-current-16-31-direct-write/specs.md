# Dirty-Page Buffer Merge Future-Current 16-31 Direct Write

## Problem

The current future-current direct-write policy publishes full index leaves and
partial leaves with `1-15` free slots during child dirty-buffer merge. The
latest free-slot detail evidence shows the remaining `51,341`
`future-current-header-partial-leaf` fallback rows split across `16-31`,
`32-63`, `64-127`, and `128+` free-slot ranges. A broad future-current
direct-write experiment regressed prepared insert to `94.432 us/op`, so the
next behavior slice should test only the nearest remaining range: `16-31` free
slots.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage policy in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB handler or SQL
  source is changed.
- `mylite_storage_commit_statement()` advances the parent current header
  before merging child dirty pages, and rollback truncates the file back to the
  stable parent header page count. Future-current pages below the current
  header and beyond the stable header are therefore rollback-protected by
  truncation.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
  requires a full parent dirty buffer, a future-current page below the parent
  current header page count, no parent or child append-buffer residency, an
  index-leaf page, and no parent dirty-buffer resident entry.
- The latest detail counters report `15,491`
  `future-current-header-partial-leaf` rows with `16-31` free slots,
  `19,321` with `32-63`, `14,523` with `64-127`, and `2,006` with `128+`.

## Design

Add a guard outcome:

- `future-current-header-16-31-direct-write`

When the existing future-current direct-write preconditions hold, direct-write
an index leaf with `16-31` free slots. Keep full leaves on
`future-current-header-direct-write`, `1-15` free-slot leaves on
`future-current-header-near-full-direct-write`, and leaves with `32+` free
slots on `future-current-header-partial-leaf` fallback replay.

The slice reuses `direct_write_dirty_page_buffer_merge_entry()` and the
existing checksum refresh path. It does not direct-write append-buffer pages,
parent-resident pages, non-leaves, branch pages, pages past the current
header, or entries merged while the parent dirty buffer is not full.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to resolve through the
MyLite storage engine.

## Single-File And Lifecycle Impact

No files are introduced. Durable state remains in the primary `.mylite` file
plus the existing MyLite-owned journal lifecycle. Direct-written `16-31`
future-current leaves are newly allocated pages below the statement current
header and are discarded by rollback truncation to the stable header page
count.

## Public API And Binary Impact

No public API changes and no dependencies. Test-hook builds gain one guard
outcome name and focused self-test expectations.

## Tests And Verification

- Extend the future-current direct-write storage self-test to prove a
  `16-31` free-slot future-current leaf direct-writes, records the new guard
  outcome, records coarse/detail free-slot bands, avoids dirty-buffer pressure,
  and shares rollback truncation with the existing future-current direct-write
  path.
- Preserve the future-page relation test proving larger partial leaves remain
  on fallback replay.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Future-current index leaves with `16-31` free slots direct-write under
  parent dirty-buffer pressure when all existing future-current guards pass.
- Future-current leaves with `32+` free slots still use fallback replay.
- Append-buffer-resident pages, branch pages, parent-resident pages, pages past
  the current header, and non-full parent buffers still use fallback replay.
- Prepared-insert evidence shows whether the targeted `16-31` class reduces
  dirty-buffer pressure without repeating the broad partial-leaf regression.

## Verification Evidence

VPS prepared-insert component evidence after implementation:

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported `68.775 us/op` for the prepared insert step.
- Direct-written dirty `index-leaf` merge rows increased to `53,136`, split
  into `3,808` `future-current-header-direct-write` full leaves, `31,202`
  `future-current-header-near-full-direct-write` leaves, and `18,126`
  `future-current-header-16-31-direct-write` leaves.
- `future-current-header-partial-leaf` fallback rows dropped to `34,484`, all
  with `32+` free slots: `18,349` with `32-63`, `14,152` with `64-127`, and
  `1,983` with `128+`.
- Dirty `index-leaf` pressure admissions from dirty-buffer merge dropped to
  `34,484`; `index-branch` pressure admissions were `287`, including `141`
  checksum-dirty branch pages.
- The final MariaDB smoke archive was `33,974,138` bytes (`32.40 MiB`).

## Risks

- `16-31` free-slot leaves may still receive later inserts that would have
  coalesced in the parent dirty buffer. Benchmark evidence decides whether the
  narrower policy is profitable enough to keep.
- The policy depends on the existing future-current rollback proof rather than
  dirty-page undo preimages for these newly allocated pages.
