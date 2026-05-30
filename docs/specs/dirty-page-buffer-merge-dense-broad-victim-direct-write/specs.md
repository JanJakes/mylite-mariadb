# Dirty-Page Buffer Merge Dense Broad Victim Direct Write

## Problem

After maintained-root writer decode removal and branch checksum deferral, the
prepared-insert profile still spends most storage work publishing dirty
index-leaf pages under parent dirty-buffer pressure. The current residual
fallback group is not safe to direct-write broadly: earlier `32-63` and
below-tail experiments reduced pressure but regressed prepared-insert timing by
discarding too much in-buffer leaf coalescing.

The remaining rejected-candidate pressure-victim matrix exposes a narrower
case: a `64-127` free-slot incoming below-tail leaf can evict a denser
`32-63` free-slot resident leaf. Direct-writing only that incoming page
preserves the denser victim without adopting the rejected broad policy.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage behavior only in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source is involved.
- `merge_dirty_page_buffer()` computes a direct-write guard outcome before
  either publishing a child dirty page or replaying it into the parent dirty
  buffer.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
  limits future-current direct writes to pages below the parent current header,
  outside parent/child append buffers, and not already resident in the parent
  dirty buffer.
- `dirty_page_buffer_pressure_flush_index()` identifies the resident dirty
  buffer victim that fallback replay would publish when the parent buffer is
  full.
- `direct_write_dirty_page_buffer_merge_entry()` writes through the existing
  checksum refresh and direct-write publication path.

## Design

Add a direct-write guard outcome:
`future-current-header-dense-broad-victim-direct-write`.

The outcome applies only when the existing future-current leaf guards have
already passed and all of the following are true:

- the incoming leaf has `64-127` free slots;
- the incoming page is `32-127` pages below the parent dirty-buffer leaf tail;
- the parent dirty buffer is full;
- the current pressure victim is checksum-dirty;
- the current pressure victim is an index leaf with `32-63` free slots.

When the outcome fires, the incoming future-current page is direct-written and
the denser resident victim remains in the parent dirty buffer. Existing
replaced-broad-victim lifecycle counters remain scoped to the older
already-replaced victim predicate; the new behavior is counted through the
guard outcome tables and the residual rejected-candidate pressure summaries.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, metadata, storage-engine
routing, or file-format behavior changes. `ENGINE=InnoDB` continues to route
through MyLite. The change only affects which internal dirty index leaf is
published during nested dirty-buffer merge pressure.

## Single-File And Lifecycle Impact

No durable sidecars, journal layout, recovery layout, or embedded lifecycle
changes. The direct write uses the existing future-current invariant: the page
is within the parent current header but beyond the stable parent statement
header, is not append-buffer resident, and rollback remains protected by
header-count truncation.

## Public API And File Format Impact

No public API or on-disk format changes.

## Storage-Engine Routing Impact

No storage-engine routing change. Supported engine names continue to route
through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. Production builds add one small guard branch and fixed
leaf free-slot metadata checks in the dirty-buffer merge path.

## Tests And Verification

- Add focused storage self-test coverage proving the new guard:
  - direct-writes a `64-127` free-slot incoming below-tail leaf;
  - preserves the `32-63` free-slot resident victim;
  - does not increment the older replaced-broad-victim matrix;
  - keeps rollback truncation protection for the future-current page.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Evidence

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `313.93 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with `33,979,762` byte (`32.41 MiB`) `libmariadbd.a`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `442.06 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed. The prepared insert step was `78.120 us/op`.

The prepared-insert profile reported:

- `2,773` dirty `index-leaf`
  `future-current-header-dense-broad-victim-direct-write` rows;
- `28,551` dirty `index-leaf` buffer-limit pressure admissions, down from
  `32,008` in the previous branch-checksum-deferral profile;
- `59,392` dirty `index-leaf` merge direct writes, up from `56,119`;
- `87,944` index-leaf dirty refreshes and `234,680` zero-tail checksum calls,
  down from `88,128` and `234,864`;
- residual rejected below-tail candidate admissions of `6,634`, with `6,627`
  buffer-limit flushes, `7` discards, and `0` clears;
- maintained-root decode sites unchanged at `677` total, concentrated in
  planning and journal validation.

## Acceptance Criteria

- The new guard fires only for `64-127` incoming leaves that would evict a
  checksum-dirty `32-63` resident victim.
- Existing full, near-full, `16-31`, replaced-broad-victim, fallback, and
  rollback behavior remains unchanged outside that predicate.
- Prepared-insert benchmark evidence shows lower dirty leaf pressure without a
  broad-direct-write regression.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- The predicate is still heuristic: `32-63` resident victims are denser, not
  semantically proven hot. Benchmark evidence must remain the gate.
- Page-id tail distance is an append-workload proxy, not a durable right-edge
  marker.
