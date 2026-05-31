# Single-Pass Dirty Pressure Selection

## Problem

The prepared-insert smoke profile still routes many hot dirty-buffer merge
decisions through `dirty_page_buffer_pressure_flush_index()`:

- `21,031` buffer-limit index-leaf flushes;
- `66,144` dirty index-leaf merge direct writes; and
- `21,031` residual broad partial fallback rows.

The pressure selector currently scans the dirty buffer once to find the maximum
resident leaf page id, then scans again from the round-robin flush position to
select the victim. The buffer is bounded to the `16`-page journal protected
window, but the selector is called repeatedly inside the prepared-insert hot
path and by direct-write guard predicates. The next safe slice is to remove the
duplicate scan without changing which victim is selected.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage code only in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL, handler,
  or storage-engine source is modified.
- `store_dirty_page_in_buffer()` calls
  `dirty_page_buffer_pressure_flush_index()` when the parent dirty-page buffer
  reaches `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT`.
- Dirty-buffer merge direct-write guards also call the same selector to inspect
  the would-be pressure victim before deciding whether to publish the incoming
  child page directly.
- The existing selector priority is:
  1. first clean index leaf in round-robin order;
  2. first full dirty index leaf in round-robin order;
  3. highest-fill valid dirty index leaf whose page id is not the maximum
     resident leaf page id; and
  4. first index leaf, or the round-robin start if no leaf exists.

## Design

Rewrite `dirty_page_buffer_pressure_flush_index()` to collect the maximum leaf
page id and the best non-full dirty leaf candidates during the round-robin
selection pass:

- keep the early return for the first clean index leaf;
- keep the first full dirty leaf slot;
- track the highest-fill dirty leaf and the second-highest-fill dirty leaf in
  scan order; and
- after the pass, choose the highest-fill candidate unless it is the maximum
  resident leaf page id, otherwise choose the second candidate if it is not the
  maximum.

Tracking the second candidate preserves the existing max-leaf exclusion when
the densest dirty leaf is also the tail leaf. Strictly greater fill comparisons
preserve the current first-in-scan-order tie behavior.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior, metadata,
storage-engine routing, wire-protocol, or file-lifecycle behavior changes.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, sidecar, or embedded lifecycle
change. The selector only chooses which already-protected dirty page publishes
when the transient dirty buffer is full.

## Public API, File Format, Binary Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Production
code replaces a bounded duplicate scan with a slightly richer single-pass
selection.

## Tests And Verification Plan

- Reuse existing dirty-page pressure self-tests, especially:
  - clean leaf preference;
  - full dirty leaf preference; and
  - highest-fill non-max dirty leaf preference, including the case where the
    highest-fill leaf is the max resident leaf.
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

- Dirty-buffer pressure tests keep the same victim-selection behavior.
- Prepared-insert structural counters stay equivalent: no new maintained-root
  decode site, no new dirty-page publication policy, and no checksum counter
  regression.
- Storage and embedded storage-engine smoke verification pass.

## Risks

- The selector's tie behavior is subtle because it scans from the round-robin
  start position. The single-pass implementation must retain strict
  better-than comparisons so equal-fill candidates still select the earlier
  scanned slot.

## Verification Result

Implemented in `packages/mylite-storage/src/storage.c` by tracking the maximum
resident index-leaf page id and the best two non-full dirty leaf fill
candidates during the existing round-robin pressure-selection pass. Existing
dirty-page pressure self-tests cover the clean-leaf, full-leaf, and high-fill
non-max leaf priorities, including the case where the densest leaf is the
maximum resident page id.

Passed:

- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  (`310.47 sec`)
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  (`327.57 sec`)
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  (`libmariadbd.a` `33,979,802` bytes, `32.41 MiB`, `478` members)
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`

The prepared-insert benchmark command
`build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
reported a `71.218 us/op` sampled step. Structural counters stayed equivalent:
`8` full-page checksum calls, `227,063` zero-tail checksum calls, `87,176`
index-leaf dirty refreshes, `21,031` dirty leaf pressure admissions, `66,144`
dirty leaf merge direct writes, `2` raw entry order builds, and `668` raw entry
order probes. Maintained-root decode sites remain limited to protected
planning/validation reads:

- `read_index_leaf_run_root`: `1` total decode, `1` full checksum, `0`
  checksum-dirty
- `plan_maintained_index_root_inserts`: `674` total decodes, `2` full
  checksum, `672` checksum-dirty
- `validate_recovery_journal_saved_page`: `2` total decodes, `2` full
  checksum, `0` checksum-dirty
