# Replaced Broad Victim Direct-Write Slot Facts

## Problem

The prepared-insert merge guard builds a broad-victim context before choosing a
replaced-broad-victim direct write. That context already has the incoming leaf
free-slot count and the selected victim leaf free-slot count. The test-hook
recorder for replaced broad-victim direct writes still reparses both pages with
`dirty_page_buffer_index_leaf_occupancy()` to derive free-slot detail bands.

The current smoke profile reports `4,218` replaced broad-victim direct-write
pages. Recomputing both leaf detail bands is redundant test-hook work.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  dirty-buffer merge counters.
- `dirty_page_buffer_merge_broad_victim_guard_outcome()` calls
  `init_dirty_page_buffer_merge_broad_victim_guard_context()` before recording
  replaced broad-victim direct-write facts.
- The context carries `incoming_free_slots`, `victim_free_slots`, and
  `victim_entry`, and the guard only enters the replaced-broad-victim recorder
  when the victim has replacements and both slot counts are in the broad-victim
  detail range.
- `record_dirty_page_buffer_merge_replaced_broad_victim_direct_write()` only
  needs incoming detail band, victim detail band, and victim replacement state
  to update the direct-write and lifecycle counters.

## Design

Compute incoming and victim free-slot detail bands from the guard context's
free-slot counts and pass them into the replaced broad-victim recorder.

Keep a parser fallback inside the recorder for any future caller that does not
have precomputed slots. The current guarded caller passes valid slots, so it
avoids reparsing both leaf pages.

Do not change guard decisions, victim selection, dirty-buffer storage,
journaling, recovery validation, checksum bytes, maintained-root planning, or
non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format, or
compatibility support status changes. This is test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer merge, direct-write publication,
statement rollback, and durable storage bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing broad-victim
guard and direct-write paths.

## Tests And Verification

- Existing storage self-tests cover replaced broad-victim direct-write matrix,
  lifecycle start, replacement, flush, discard, and clear counters.
- Verified:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
    passed in `327.38 sec`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
    passed in `329.93 sec`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
    produced a static archive of `33,989,146` bytes with `478` members
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert benchmark reported `73.393 us/op` for the prepared insert
step under variable host load. Replaced broad-victim direct-write rows stayed
unchanged at `4,218` pages, split across the same incoming/victim free-slot
detail and preserved-victim replacement-state matrix. Lifecycle start,
replacement, flush, discard, and clear rows stayed unchanged.

The structural counters stayed unchanged: `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `677` maintained-root decodes, `87,178`
dirty-page-flush checksum refreshes, `21,031` pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, and `122,388` future-page
relation rows.

## Acceptance Criteria

- Replaced broad-victim direct-write, lifecycle start, replacement, flush,
  discard, and clear rows stay unchanged.
- Guard outcome, pressure admission, fallback admission, replacement,
  publication, checksum, pressure-context, planned-store, and maintained-root
  decode counters stay unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- The recorder must not assume future callers have precomputed slots. Preserve
  a fallback parse path when either slot is unavailable.
