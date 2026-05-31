# Pressure Incoming Guard Facts

## Problem

Prepared-insert merge fallback stores pressure-admitted pages after the merge
guard has already classified the child page family and leaf occupancy. The
pressure incoming recorder still calls
`test_dirty_page_buffer_flush_page_family()` and
`dirty_page_buffer_index_leaf_occupancy()` for the same incoming page.

The current smoke profile reports `21,031` pressure admissions, all
`index-leaf`, and those admissions preserve the same leaf fill/free-slot detail
rows already computed by the guard. Repeating the family and occupancy parses is
redundant test-hook work.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  pressure-admission counters.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` fills
  `mylite_storage_test_dirty_page_buffer_merge_guard_facts::family` and
  `::occupancy` for merge entries while deciding direct-write versus fallback.
- `record_dirty_page_buffer_pressure_incoming_page()` records pressure admission
  family, source, replacement-state, leaf fill, free-slot, free-slot-detail, and
  write-site counters from the same page bytes.
- Direct tests call the recorder without merge guard facts, so the old parse
  fallback must remain available.

## Design

Add scoped test-hook-only pressure incoming facts for page family and leaf
occupancy. In `merge_dirty_page_buffer()`, save the prior pressure incoming
facts, publish the guard facts while the fallback store runs, then restore the
prior facts.

Teach `record_dirty_page_buffer_pressure_incoming_page()` to use the scoped
facts when present and fall back to its existing family and occupancy parsers
otherwise. Explicit non-leaf occupancy facts remain valid and suppress leaf
tables without another parse.

Do not change merge guard decisions, pressure victim choice, dirty-buffer
storage, journaling, recovery validation, checksum bytes, maintained-root
planning, or non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format, or
compatibility support status changes. This is test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer pressure admission, fallback buffering,
statement rollback, and durable storage bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing pressure
admission path.

## Tests And Verification

- Existing storage self-tests cover pressure incoming family, dirty family,
  source, replacement-state, write-site, fill band, free-slot band, and
  free-slot-detail counters.
- Verified:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
    passed in `301.51 sec`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
    passed in `355.89 sec`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
    produced a static archive of `33,989,146` bytes with `478` members
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert benchmark reported `104.566 us/op` for the prepared insert
step under variable host load. Pressure incoming rows stayed unchanged:
`21,031` incoming pages, all `index-leaf` and checksum-dirty, all attributed to
`dirty-buffer-merge`, `never-replaced`, and the
`insert_branch_index_leaf_entry` pressure write site. Leaf detail rows stayed
`11,623` `32-63`, `7,430` `64-127`, and `1,978` `128+`.

The structural counters stayed unchanged: `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `677` maintained-root decodes, `87,178`
dirty-page-flush checksum refreshes, `21,031` pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, and `122,388` future-page
relation rows.

## Acceptance Criteria

- Pressure incoming family, dirty family, source, replacement-state, write-site,
  fill band, free-slot band, and free-slot-detail rows stay unchanged.
- Guard outcome, future-page relation, fallback admission, replacement,
  publication, checksum, pressure-context, planned-store, and maintained-root
  decode counters stay unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- Scoped pressure incoming facts must not leak beyond the store call. Save and
  restore all fact fields around the fallback store.
