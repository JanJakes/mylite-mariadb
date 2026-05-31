# Maintained Root Plan Dirty Read Copy Elision

## Problem

The prepared-insert profile still reports `1,346` dirty-buffer direct-read
copies of `index-root` pages even after writer-side maintained-root decodes and
checksum refreshes were removed. Maintained-root planning must continue to
decode and validate dirty root bytes, but it does not need a full-page copy when
the dirty-buffer entry is already the local checksum-dirty root image that the
planner will decode read-only.

The safe slice is to pass a const page reference from the dirty-buffer entry
into maintained-root planning decode while keeping the existing scratch page
for durable reads and non-root fallback.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is modified.
- `plan_maintained_index_root_inserts()` calls
  `read_maintained_index_root_plan_page()` and then decodes the returned root
  bytes with `decode_maintained_index_root_page_with_checksum_state()`.
- The current plan read helper gets active dirty-buffer pages through
  `copy_dirty_page_buffer_undo_preimage()`, which copies the full 4 KiB page
  into a stack scratch page before the planner decodes it read-only.
- The existing dirty-read path already preserves checksum-dirty state and only
  accepts dirty maintained-root pages for checksum-dirty root planning. Dirty
  branch pages that cannot be decoded without checksum refresh still fall back
  to the durable read path.

## Design

Change maintained-root plan reads to return a const page pointer:

- keep the caller-owned scratch page for durable pager reads and fallback
  branches;
- when an active dirty-buffer entry exists and is either checksum-valid or a
  checksum-dirty maintained root, return a const pointer to the resident dirty
  page and the existing checksum-dirty flag;
- decode the root from that const pointer exactly as before, preserving planning
  validation and checksum-dirty validation behavior;
- keep non-root, checksum-dirty dirty-buffer entries on the durable fallback
  path;
- leave writer-side dirty page reads unchanged when the writer needs a mutable
  page image to mark or rewrite.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, file-format, or durable byte changes.

## Single-File And Lifecycle Impact

No file lifecycle, journal, recovery, lock, sidecar, or embedded lifecycle
change. Dirty-buffer ownership remains with the active statement chain, and the
planner only borrows the pointer for immediate read-only decode.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Test-hook
benchmark output should show fewer `index-root` direct-read dirty copies through
the existing dirty-page copy context counters.

## Tests And Verification Plan

- Add focused storage self-test coverage that stages a checksum-dirty
  maintained root in the dirty buffer, reads it through the maintained-root
  planning helper, verifies the returned page pointer is the resident dirty
  page, decodes it with checksum-dirty state, and asserts the direct-read dirty
  copy counter does not increment.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert benchmark output shows lower `index-root` direct-read dirty
  copy counts while maintained-root decode sites remain at the protected
  validation count.
- Full-page checksum calls, zero-tail checksum calls, dirty refresh counts, and
  durable file behavior remain equivalent.
- Storage and embedded storage-engine smoke verification pass.

## Verification Results

- `git diff --check` passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `build/dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  `304.45 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `315.40 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed; `libmariadbd.a` was `33,982,994` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed. The benchmark reported `674` `index-root` direct-read dirty copies,
  down from `1,346`. Maintained-root decode sites stayed at `677`, full-page
  checksum calls stayed at `8`, zero-tail checksum calls stayed at `227,063`,
  and the sampled prepared insert step was `74.431 us/op`.

## Risks

- The borrowed pointer is valid only while the active dirty-buffer entry remains
  resident. The planner must use it for immediate decode and must not retain the
  pointer in the plan.
