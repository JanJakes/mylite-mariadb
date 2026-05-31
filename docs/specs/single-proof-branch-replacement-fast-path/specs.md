# Single-Proof Branch Replacement Fast Path

## Problem

The current prepared-insert smoke profile reports `130,313` dirty-buffer
`index-branch` replacements. The dominant classes are already handled by
in-place fast paths:

- `115,753` entry-count-only branch replacements; and
- `14,172` entry-count-plus-fence branch replacements.

The implementation still proves those classes through two separate helpers.
An entry-count-plus-fence replacement first fails the entry-count-only helper,
then reruns branch metadata, fixed-header, child page-id, fence, and tail-byte
checks in the fence helper. The next safe slice is to keep the same byte-level
proof while avoiding the duplicated branch-layout validation and fixed-prefix
comparisons.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code only in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler,
  or storage-engine source is modified.
- `store_dirty_page_in_buffer_at_pressure_write_site()` sees the resident
  dirty-buffer page and replacement page before the resident slot is
  overwritten.
- `replace_dirty_page_buffer_branch_entry_count_only()` proves that the
  checksum and page-owned entry-count fields are the only changed bytes.
- `replace_dirty_page_buffer_branch_entry_count_and_fences()` proves the
  checksum, page-owned entry-count, and existing child fence row/key fields are
  the only changed bytes.
- Both helpers validate the same branch page family, key size, child count,
  used-byte layout, fixed header ranges, and changed entry-count field before
  applying an in-place update.

## Design

Replace the two branch replacement helpers with one combined helper:

- validate both pages are index-branch pages;
- validate key size, child count, used bytes, child capacity, fixed offsets,
  and fixed header bytes once;
- reject replacements whose page-owned entry-count field is unchanged;
- preserve the existing entry-count-only fast case with one payload/tail byte
  comparison before updating only checksum and entry count;
- when the payload/tail comparison differs, prove the existing
  entry-count-plus-fence case by verifying all child page ids and tail bytes
  are unchanged while at least one child fence row/key field changed; and
- keep structural, invalid, identical, or fence-only replacements on the
  existing full-page copy fallback.

The combined helper still writes the same resident dirty-buffer page image as
the previous fast paths and increments the same test-hook counters for the
class that matched.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, or file-lifecycle behavior changes.
Eligible branch replacements produce the same buffered bytes as the previous
two-helper implementation.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change. The slice only changes transient dirty-buffer replacement proof before
eventual journal-protected publication.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Production
code removes one branch helper and centralizes the branch replacement proof in
one local helper.

## Tests And Verification Plan

- Reuse existing dirty-buffer branch fast-path self-tests:
  - entry-count-only branch replacement updates checksum and entry-count in
    place;
  - entry-count-plus-fence branch replacement updates checksum, entry-count,
    and child fence in place; and
  - structural child-id changes keep the full-copy fallback.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Entry-count-only and entry-count-plus-fence branch fast-path counters remain
  aligned with the branch replacement change-class table.
- Structural branch replacements still fall back to the full page copy path.
- Prepared-insert checksum, dirty publication, and maintained-root decode
  counters do not regress.
- Storage and embedded storage-engine smoke verification pass.

## Risks

- The helper must not weaken the byte-level proof. Any changed branch child
  count, child page id, fixed metadata byte, or unused tail byte must still
  reject the fast path and use the full-copy fallback.

## Verification Result

Implemented in `packages/mylite-storage/src/storage.c` by replacing the two
branch replacement fast-path helpers with one
`replace_dirty_page_buffer_branch_entry_count_or_fences()` helper. The helper
validates the common branch layout once, preserves the entry-count-only
payload/tail equality proof, and only applies the fence fast path after proving
child page ids and unused tail bytes are unchanged.

Passed:

- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  (`319.79 sec`)
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  (`328.14 sec`)
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  (`libmariadbd.a` `33,979,802` bytes, `32.41 MiB`, `478` members)
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`

The prepared-insert benchmark command
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
reported a `70.064 us/op` sampled step. Structural counters stayed equivalent:
`115,753` branch entry-count fast replacements, `14,172` branch
entry-count-fence fast replacements, `34,548` leaf growth fast replacements,
`8` full-page checksum calls, `227,063` zero-tail checksum calls, `21,031`
dirty leaf pressure admissions, and `66,144` dirty leaf merge direct writes.
Maintained-root decode sites stayed limited to protected planning/validation
reads: `read_index_leaf_run_root` (`1`), `plan_maintained_index_root_inserts`
(`674`), and `validate_recovery_journal_saved_page` (`2`).
