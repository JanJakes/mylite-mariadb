# Dirty-Page Merge Guard Context

## Problem

The current prepared-insert profile still spends most insert-loop checksum work
publishing dirty index leaves:

- `87,176` index-leaf dirty refreshes;
- `66,144` dirty index-leaf merge direct writes;
- `21,031` buffer-limit index-leaf flushes; and
- `21,031` residual `future-current-header-partial-leaf` fallback rows.

The remaining maintained-root decodes are protected gates:
`plan_maintained_index_root_inserts`, `validate_recovery_journal_saved_page`,
and one durable root read. This slice must not remove those decodes.

The merge guard now tests several future-current leaf predicates in sequence.
For broad partial leaves those predicates repeatedly derive the same transient
facts: incoming leaf free slots, parent leaf tail distance, and the would-be
pressure victim. The parent dirty buffer is bounded to the journal protected
page window, but the prepared-insert benchmark calls this path more than
`120,000` times, so repeated scans still sit on the hot path.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- Prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB storage-engine
  behavior is changed.
- `merge_dirty_page_buffer()` asks
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` to choose
  direct publication or fallback replay for each child dirty-buffer entry.
- `dirty_page_buffer_pressure_flush_index()` selects the resident page that
  fallback replay would evict when the parent dirty buffer is full.
- Existing direct-write predicates cover full leaves, near-full leaves,
  `16-31` free-slot leaves, replaced broad victims, dense/equal broad victims,
  equal dense victims, and wider victims.

## Design

Replace the repeated broad-victim predicate scans with one transient guard
context:

1. Compute incoming leaf free slots once for broad future-current leaves.
2. Compute whether the incoming page is `32-127` pages below the parent dirty
   buffer leaf tail once.
3. Compute the pressure victim index once and expose the resident victim entry
   and victim free slots through the context.
4. Preserve the existing predicate priority:
   replaced broad victim, dense broad victim, equal broad victim, equal dense
   victim, then wider victim.
5. Keep full, near-full, and `16-31` fast guards before the broad context.

The refactor does not change which pages direct-write or fallback. It only
removes redundant transient scans and field decodes inside one guard decision.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, or file-lifecycle behavior changes.
Supported routed engines still use the same MyLite storage layer.

## Single-File And Lifecycle Impact

No durable sidecar, journal layout, page format, recovery layout, lock, or
embedded lifecycle change. Dirty pages are still protected before publication,
and rollback continues to use the existing dirty undo/header-count rules.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, file-format, dependency, or license change. Production code adds
a small stack-only context struct and removes several repeated helper calls
from the hot guard path.

## Tests And Verification Plan

- Reuse the existing focused dirty-buffer merge self-tests for every guard
  outcome to prove decisions stay unchanged.
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

- Guard outcome counters remain structurally equivalent for prepared inserts:
  no new maintained-root decode site and no new broad publication policy.
- Existing direct-write guard self-tests pass unchanged.
- The prepared-insert benchmark does not show a structural regression in dirty
  page checksum publication counts.
- Storage and embedded storage-engine smoke verification pass.

## Verification Result

Verified on 2026-05-31 in the `custom-storage` worktree:

- `cmake --build --preset dev --target mylite_storage_test`: passed before
  the benchmark and again after formatting.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `307.47 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. The prepared insert step sampled `72.130 us/op`; full-page checksum
  calls stayed at `8`, zero-tail checksum calls stayed at `227,063`,
  index-leaf dirty refreshes stayed at `87,176`, dirty leaf pressure
  admissions stayed at `21,031`, dirty leaf merge direct writes stayed at
  `66,144`, raw entry order builds stayed at `2`, and raw entry order probes
  stayed at `668`.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `371.48 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,979,970` byte (`32.41 MiB`) `libmariadbd.a`.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.

Maintained-root decode sites remained protected validation/read gates:

- `read_index_leaf_run_root`: `1` decode, `1` full checksum;
- `plan_maintained_index_root_inserts`: `674` decodes, `2` full checksum,
  `672` checksum-dirty; and
- `validate_recovery_journal_saved_page`: `2` decodes, `2` full checksum.

## Risks

- Wall-clock prepared-insert timing is host-noise sensitive, so this slice uses
  correctness tests plus structural benchmark counters as the primary gate.
- The refactor must preserve predicate priority exactly; changed priority would
  alter test-hook attribution and possibly dirty-buffer coalescing behavior.
