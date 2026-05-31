# Dirty-Page Merge Planned Pressure Victim

## Problem

The prepared-insert profile now limits maintained-root decodes to protected
planning, recovery-journal validation, and one root-to-leaf read gate. The
remaining writer-side work around the broad future-current merge path is dirty
buffer inspection: the broad guard computes the parent dirty-buffer pressure
context to decide whether an incoming leaf can direct-write, then a partial
fallback leaf immediately asks `store_dirty_page_in_buffer()` to recompute the
same pressure victim before replaying into the parent buffer.

The next safe slice is to carry the broad guard's planned pressure victim into
the fallback writer for that same child entry. This removes the redundant
writer-side victim-selection scan without weakening planning or journal
validation.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler, or
  storage-engine source is modified.
- `merge_dirty_page_buffer()` iterates child dirty-buffer entries and asks
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` whether each
  entry can direct-write or must replay into the parent dirty buffer.
- Broad future-current outcomes use
  `dirty_page_buffer_pressure_complete_flush_context()` to compute both the
  parent dirty-buffer leaf tail and would-be pressure victim.
- When a broad guard returns
  `future-current-header-partial-leaf`, parent dirty-buffer state is unchanged
  between guard evaluation and fallback replay.
- Fallback replay through `store_dirty_page_in_buffer()` recomputes the pressure
  victim with `dirty_page_buffer_pressure_flush_index()` before flushing and
  reusing that slot.

## Design

Add a per-entry merge pressure context plan:

- pass a stack-owned plan through
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` and the
  broad guard helpers;
- when the broad guard computes a complete pressure context, store the selected
  flush index in that plan;
- when the same entry falls back, use the planned flush index in a narrow
  full-buffer store helper instead of calling
  `dirty_page_buffer_pressure_flush_index()` again; and
- fall back to the existing store path if the parent buffer is no longer full,
  the planned index is out of range, or the target page is already resident.

Normal pressure admission still uses `dirty_page_buffer_pressure_flush_index()`.
The planned path is only for the immediate broad fallback writer after the guard
has already selected the same victim from unchanged parent state.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, or file-lifecycle behavior changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change. The plan stores only transient facts derived from already-protected
parent dirty-buffer entries and is dropped before the next child entry.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Production
code adds one narrow planned full-buffer store helper. Test-hook builds add
small scalar counters for pressure-context builds and planned stores.

## Tests And Verification Plan

- Extend broad direct-write self-test coverage to keep proving a clean pressure
  victim before a later tail does not hide the tail evidence.
- Add focused self-test coverage where a broad future-current partial leaf uses
  the planned pressure victim for fallback replay.
- Reuse existing direct-write guard tests for protected, replaced, dense, equal,
  equal-dense, wider, and fallback outcomes.
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

- Prepared-insert structural counters remain equivalent: dirty leaf pressure
  admissions, merge direct writes, checksum calls, and maintained-root decode
  sites do not regress.
- The new self-test proves broad fallback replay uses the planned victim while
  preserving the incoming dirty-buffer entry and flushed victim bytes.
- Benchmark output reports planned pressure stores for the residual broad
  fallback path.
- Storage and embedded storage-engine smoke verification pass.

## Verification Result

- `cmake --build --preset dev --target mylite_storage_test` passed.
- `build/dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed in
  `307.08 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_storage_test mylite_embedded_storage_engine_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `342.91 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC` passed; the
  static embedded archive was `33,980,970` bytes (`32.41 MiB`) with `478`
  members.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  passed.

The prepared-insert benchmark
`build/storage-smoke-dev/tools/mylite_perf_baseline
--phase=prepared-insert-components 1000 100000` reported the same durable
structural counters as the prior slice: `21,031` dirty leaf pressure
admissions, `66,144` dirty leaf merge direct writes, `87,176` index-leaf dirty
refreshes, `8` full-page checksum calls, `227,063` zero-tail checksum calls,
and `677` maintained-root decodes. Maintained-root decode sites remained
limited to `read_index_leaf_run_root` (`1`), `plan_maintained_index_root_inserts`
(`674`), and `validate_recovery_journal_saved_page` (`2`). The new pressure
context output reported `31,938` builds and `19,053` planned stores. The
sampled prepared insert step was `71.366 us/op` while the host load average was
around `17`, so the wall-time sample is retained as evidence but the structural
counters are the stable comparison point for this slice.

## Risks

- Using a planned victim after parent dirty-buffer mutation would be stale. The
  plan is per child entry and is discarded before the next entry.
- The planned full-buffer helper duplicates the existing pressure-store branch.
  It must keep the same flush, incoming-page accounting, bucket relink, and
  `next_flush_index` updates as the generic path.
