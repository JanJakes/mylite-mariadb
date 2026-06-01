# Maintained Root Dirty Plan Facts

## Problem

The current prepared-insert benchmark keeps `677` maintained-root decodes after
the earlier writer-side elision slices. The remaining decode table is:

- `1` decode and full checksum under `read_index_leaf_run_root`;
- `674` decodes under `plan_maintained_index_root_inserts`, with `672` over
  checksum-dirty roots already resident in the active dirty buffer;
- `2` decodes and full checksums under `validate_recovery_journal_saved_page`.

The full-checksum rows are protected validation boundaries and must remain.
The safe target is the repeated planning decode of checksum-dirty dirty-buffer
root pages that were produced by earlier maintained-root writer operations in
the same active statement chain.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own this
  storage layer.
- `plan_maintained_index_root_inserts()` reads maintained root pages through
  `read_maintained_index_root_plan_page()` and decodes roots with
  `decode_maintained_index_root_page_with_checksum_state()`.
- For checksum-dirty dirty-buffer roots, the decode skips checksum calculation
  but still revalidates the root header, root entry count, used bytes, overflow
  tail, row references, and sort order on every planning pass.
- `insert_maintained_index_root_entry_in_dirty_buffer()` and
  `mark_maintained_index_root_overflow_tail_in_dirty_buffer()` already validate
  the planned target against the resident bytes, then mutate the current
  statement dirty-buffer entry and leave the checksum dirty.
- Fallback maintained-root writers also validate the page before storing it
  through `pager_write_buffered_maintained_index_page()`.
- `validate_recovery_journal_saved_page()` must continue decoding saved
  maintained-root pages from the protected journal image.

## Design

Carry validated maintained-root facts on dirty-buffer entries only after a
maintained-root writer has successfully validated and mutated or stored that
entry. The facts record the page id through the dirty-buffer entry itself plus
root table id, index number, key size, entry count, used bytes, flags, and
overflow-tail page id.

Generic dirty-buffer page admission or replacement clears maintained-root facts
so manually stored or unrelated root-looking pages cannot bypass decode. The
maintained-root writer paths repopulate the facts after successful validation.
Child-to-parent dirty-buffer merge publication copies the facts only when the
destination parent entry receives the same page id and exact page bytes from a
child entry that already carries writer-validated facts; direct-write merge
publication still goes to the durable checksum path.

`read_maintained_index_root_plan_page()` will return the resident dirty-buffer
entry when it returns a dirty entry page. `plan_maintained_index_root_inserts()`
will reuse the carried facts only when:

- the page is still a maintained-root page;
- the entry's page id and root header fields still match the carried facts;
- the overflow-tail page id remains addressable under the current header.

If any condition fails, planning falls back to the existing decode path. Durable
pages, clean dirty-buffer pages without carried facts, generic dirty entries,
branch roots, `read_index_leaf_run_root()`, and recovery-journal validation keep
their current decoders.

Do not change write-journal setup, protected-page selection, root insert
position validation in the writer, checksum publication, dirty-buffer eviction,
or the durable file format.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, transaction, recovery, or compatibility support status
changes.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty maintained-root pages are still protected by
the existing journal before publication and still receive durable checksums at
the existing dirty-buffer publication points.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license changes. Dirty-buffer
entries gain a few in-memory maintained-root fact fields.

## Tests And Verification Plan

- Extend focused storage self-tests to prove maintained-root writer mutation
  sets carried dirty plan facts and that planning can reuse those facts without
  recording another maintained-root decode site, including after child-to-parent
  dirty-buffer merge publication.
- Existing storage rollback, recovery, locking, dirty-buffer, and
  storage-engine smoke tests cover unchanged publication and recovery behavior.
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

- `plan_maintained_index_root_inserts` maintained-root decodes fall from `674`
  to the remaining durable validation count while `read_index_leaf_run_root`
  and `validate_recovery_journal_saved_page` remain visible.
- Full-page checksum calls stay at the protected validation count, and
  zero-tail checksum calls, dirty-refresh counts, pressure admissions, merge
  direct writes, and dirty-buffer replacement evidence remain equivalent.
- Storage and embedded storage-engine smoke verification pass.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `444.42 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `337.44 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size `33,991,930` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step `74.489 us/op`; `plan_maintained_index_root_inserts`
  fell from `674` decodes (`2` full checksum, `672` checksum-dirty) to `2`
  decodes (`2` full checksum, `0` checksum-dirty), leaving only `5` total
  maintained-root decodes across the protected call sites.
- The benchmark kept `8` full-page checksum calls, `227,063` zero-tail checksum
  calls, `21,031` dirty leaf pressure admissions, `66,144` dirty leaf merge
  direct writes, `87,176` index-leaf dirty refreshes, `31,938`
  pressure-context builds, and `19,053` planned stores.

## Risks

- Carried facts must be cleared on every generic dirty-buffer page mutation and
  repopulated only after maintained-root writer validation succeeds.
- The optimization must never hide durable-page corruption or recovery-journal
  corruption. Those paths keep the existing decoders.
- A future maintained-root writer path that mutates a dirty-buffer root must set
  or clear the carried facts with the same discipline.
