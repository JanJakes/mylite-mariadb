# Broad Guard Single-Pass Pressure Context

## Problem

The prepared-insert smoke profile still routes the residual broad
future-current merge path through the dirty-buffer pressure selector:

- `21,031` dirty `index-leaf` pressure admissions;
- `66,144` dirty `index-leaf` merge direct writes; and
- `21,031` residual `future-current-header-partial-leaf` fallback rows.

The broad future-current guard needs two facts about the same full parent dirty
buffer: the maximum resident index-leaf page id and the would-be pressure
victim. It currently scans the parent buffer once for the max leaf page id and
then calls `dirty_page_buffer_pressure_flush_index()`, which scans the same
bounded window again. The next safe slice is to compute both facts in one
complete scan for that guard without changing the normal pressure selector or
direct-write policy.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code only in
  `packages/mylite-storage/src/storage.c` and storage self-test wiring in
  `packages/mylite-storage/tests/storage_test.c`; no upstream MariaDB SQL,
  handler, or storage-engine source is modified.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` reaches the
  broad guard only for future-current, non-append-buffer, non-parent-resident
  index leaves with at least `32` free slots.
- `init_dirty_page_buffer_merge_broad_victim_guard_context()` validates the
  incoming broad leaf, finds the parent dirty-buffer leaf tail, checks the
  incoming leaf is `32..127` pages below that tail, and then inspects the
  pressure victim selected by `dirty_page_buffer_pressure_flush_index()`.
- The pressure selector's victim priority is:
  1. first clean index leaf in round-robin order;
  2. first full dirty index leaf in round-robin order;
  3. highest-fill valid dirty index leaf whose page id is not the maximum
     resident leaf page id; and
  4. first index leaf, or the round-robin start if no leaf exists.

## Design

Keep the normal pressure selector's early-return implementation intact and add
a complete-scan context helper for the broad guard:

- normal pressure flush calls keep the existing first-clean-leaf early return
  and still receive only the selected flush index;
- broad guard context calls the complete-scan helper and receives both the
  selected flush index and the maximum resident leaf page id in one pass;
- complete-scan mode records the first clean leaf instead of returning
  immediately, so clean-leaf victim priority is preserved while the later max
  leaf can still be seen; and
- the high-fill non-max selection keeps the existing strict comparison and
  second-best candidate behavior so round-robin tie semantics stay unchanged.

No guard outcome or dirty-page publication policy changes. The broad guard only
removes a redundant scan before applying the same existing predicates.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, or file-lifecycle behavior changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change. The slice only changes transient dirty-buffer inspection before an
already journal-protected dirty page is either preserved or published.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Production
code adds one small local context helper and reuses the existing pressure
selection rules.

## Tests And Verification Plan

- Reuse existing dirty-page pressure self-tests for clean-leaf, full-leaf, and
  high-fill non-max victim selection.
- Reuse existing broad future-current guard tests for replaced, dense, equal,
  and wider victim direct-write outcomes.
- Add focused self-test coverage where the pressure victim is the first clean
  leaf but the max resident leaf appears later in the parent dirty buffer. This
  proves the broad guard's complete scan does not lose tail-distance evidence
  while preserving clean-leaf victim priority.
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

- Broad future-current direct-write outcome counts stay structurally
  equivalent in the prepared-insert benchmark.
- Dirty leaf pressure admissions, merge direct writes, checksum counters, and
  maintained-root decode sites do not regress.
- The added self-test proves broad guard context still sees a later parent leaf
  tail when the pressure victim is an earlier clean leaf.
- Storage and embedded storage-engine smoke verification pass.

## Risks

- The selector has subtle priority and tie behavior. The context helper must
  preserve clean-leaf, full-leaf, high-fill non-max, and first-leaf fallback
  ordering exactly.
- If complete-scan mode accidentally used the normal early return, a clean
  victim before the parent leaf tail would hide the tail-distance proof and
  change broad guard outcomes.

## Verification Result

Implemented in `packages/mylite-storage/src/storage.c` by adding
`dirty_page_buffer_pressure_complete_flush_context()` for the broad guard while
leaving `dirty_page_buffer_pressure_flush_index()` on the existing early-return
path. Added
`mylite_storage_test_dirty_page_buffer_merge_broad_guard_scans_tail_after_clean_victim()`
to prove a first clean pressure victim does not hide a later parent leaf tail
from the broad guard.

Passed:

- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test` (`4m58.979s`)
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  (`310.23 sec`)
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  (`69.676 us/op` cleanest sampled prepared insert step; later samples under
  unrelated host load varied higher with unchanged structural counters)
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  (`325.55 sec`)
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  (`libmariadbd.a` `33,980,290` bytes, `32.41 MiB`, `478` members)
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`

The prepared-insert benchmark kept structural counters unchanged: `115,753`
branch entry-count fast replacements, `14,172` branch entry-count-fence fast
replacements, `34,548` leaf growth fast replacements, `8` full-page checksum
calls, `227,063` zero-tail checksum calls, `21,031` dirty leaf pressure
admissions, and `66,144` dirty leaf merge direct writes. Maintained-root decode
sites stayed limited to protected planning/validation reads:
`read_index_leaf_run_root` (`1`), `plan_maintained_index_root_inserts` (`674`),
and `validate_recovery_journal_saved_page` (`2`).
