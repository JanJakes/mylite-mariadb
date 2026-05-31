# Maintained Root Overflow Replacement Fast Path

## Problem

The prepared-insert profile now fast-replaces `666` of `668`
checksum-dirty maintained-root dirty-buffer replacements. The remaining two
`index-root` replacements use the full-page copy fallback even though the
overflow-tail marking path changes only root flags, the overflow tail page id,
the checksum slot, and the dirty flag after planning has already validated the
root page shape.

The safe slice is to prove that overflow-tail mark shape inside the dirty-buffer
replacement path and update only the changed root header fields in place.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is modified.
- `write_maintained_index_root_overflow_flags()` reads the planned dirty root
  page, calls `mark_maintained_index_root_overflow_tail()` with checksum
  refresh disabled, and writes the root through the buffered maintained page
  path as checksum-dirty.
- `mark_maintained_index_root_overflow_tail()` validates the planned root and
  then only sets `HAS_OVERFLOW_TAIL`, writes the first fallback page id, and
  clears the checksum slot.

## Design

Add a maintained-root overflow-tail dirty-buffer replacement proof:

- require resident and incoming pages to be maintained index roots;
- require matching key size, entry count, used bytes, fixed root metadata,
  payload bytes, and unused tail bytes;
- require the resident root to lack `HAS_OVERFLOW_TAIL`, the incoming root to
  add exactly that flag, the resident overflow tail page id to be `0`, and the
  incoming tail page id to be greater than the root page id;
- copy only the flags, overflow-tail page id, and checksum fields to the
  resident dirty-buffer entry, and preserve the incoming checksum-dirty state;
- fall back to the existing full-page replacement path if any proof fails.

Planning decodes, recovery-journal validation, root overflow target validation,
and durable checksum publication stay unchanged.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, file-format, or durable byte changes.

## Single-File And Lifecycle Impact

No file lifecycle, journal, recovery, lock, sidecar, or embedded lifecycle
change. The root remains checksum-dirty until the existing publication path
refreshes it.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Test-hook
builds gain one scalar counter and benchmark row for maintained-root overflow
fast replacements.

## Tests And Verification Plan

- Add focused storage self-test coverage that stores a root page in the dirty
  buffer, replaces it with an overflow-tail-marked root page, verifies the
  resident bytes match the incoming page, and asserts the new fast replacement
  counter increments.
- Keep existing maintained-root insert fast-path coverage.
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

- Prepared-insert benchmark output reports maintained-root overflow fast
  replacements for the two residual root replacements.
- Full-page checksum calls, zero-tail checksum calls, and protected
  maintained-root decode totals remain equivalent.
- Storage and embedded storage-engine smoke verification pass.

## Verification Results

- `git diff --check` passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `build/dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  `324.57 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `432.49 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed; `libmariadbd.a` was `33,983,186` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed. The benchmark reported `666` maintained-root insert fast
  replacements and `2` maintained-root overflow fast replacements, covering all
  `668` checksum-dirty `index-root` dirty-buffer replacements. Full-page
  checksum calls stayed at `8`, zero-tail checksum calls stayed at `227,063`,
  maintained-root decode sites stayed at `677`, and the sampled prepared insert
  step was `77.002 us/op`.

## Risks

- The helper is only valid for the planned no-tail to overflow-tail header
  transition. Any payload change, entry-count change, tail removal, or unrelated
  flag rewrite must keep using the full-page fallback.
