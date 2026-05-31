# Merge Fallback Pressure-Victim Facts

## Problem

The prepared-insert profile still reports `121` residual rejected below-tail
direct-write candidate pressure-victim rows. The merge fallback path already
stores the admitted leaf free-slot detail in fallback-origin state, and buffer
pressure already selects the victim entry before the flush. The test-hook
pressure-victim recorder still reparses the incoming fallback leaf and the
selected victim leaf to populate those diagnostic matrices.

This is redundant attribution work in the prepared-insert counter build. It is
not a durable checksum or validation gate.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  dirty-buffer merge counters.
- `merge_dirty_page_buffer()` computes guard facts for each child dirty-buffer
  entry, stores fallback-origin state for replayed leaves, and carries the
  admitted leaf free-slot detail through
  `test_dirty_page_buffer_merge_fallback_origin_leaf_free_slot_detail_slot`.
- `store_dirty_page_in_buffer_at_pressure_write_site()` and
  `store_dirty_page_in_buffer_with_planned_pressure_flush_index()` identify the
  buffer-limit victim before flushing it.
- `record_dirty_page_buffer_merge_fallback_leaf_pressure_victim()` currently
  reparses the incoming page to recover the free-slot detail and reparses the
  selected victim page to classify family, fill band, free-slot detail,
  replacement state, and page-id rank.

## Design

Reuse the fallback-origin incoming free-slot detail slot when it is already
available. Keep the existing incoming-page parser as a fallback for direct
tests and future callers without fallback-origin facts.

Let the buffer-limit caller compute the selected victim page family and
occupancy once and pass those facts to the pressure-victim recorder. The
recorder keeps its page-family and occupancy parser fallback when those facts
are not supplied.

Do not change dirty-buffer pressure selection, direct-write policy, dirty-page
publication, checksum refresh, journaling, recovery validation, maintained-root
planning, or non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
metadata, transaction, or compatibility support status changes. This is
test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-page undo, journal protection, statement
rollback, buffer-limit publication, commit flushes, and recovery keep the same
page bytes and validation gates.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing storage
paths.

## Tests And Verification Plan

- Existing storage self-tests and storage-smoke coverage exercise dirty-buffer
  merge fallback, pressure victims, rejected below-tail candidate matrices,
  dirty-page flushes, rollback, and recovery.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `327.89 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `435.64 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` was `33,989,146` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with `68.693 us/op` prepared insert step.

Current `prepared-insert-components 1000 100000` counters:

- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- maintained-root decodes: `677`;
- dirty-page-flush refreshes: `87,178`;
- buffer-limit pressure admissions: `21,031`;
- merge direct writes: `66,144`;
- index-leaf dirty refreshes: `87,176`;
- pressure-context builds: `31,938`;
- planned stores: `19,053`;
- future-page relation rows: `122,388`;
- rejected below-tail candidate admissions: `121`;
- rejected-candidate pressure victims: `121`.

## Acceptance Criteria

- Rejected below-tail pressure-victim family, replacement-state, fill-band,
  free-slot detail, matrix, and rank rows stay unchanged.
- Dirty-buffer pressure admissions, merge direct writes, dirty checksum
  refreshes, checksum call-site counters, maintained-root decodes, and planned
  store counters stay unchanged.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- The recorder must continue to work for direct tests or future callers that
  have not supplied precomputed facts. Preserve parser fallback for both
  incoming and victim pages.
