# Dirty-Page Buffer Merge Equal Broad Victim Direct Write

## Problem

After the dense broad-victim direct-write slice, the prepared-insert profile
still reports `28,551` dirty `index-leaf` buffer-limit pressure admissions and
`87,944` index-leaf dirty refreshes. The remaining rejected below-tail
candidate matrix shows a narrow residual class: `64-127` free-slot incoming
future-current leaves still evict `64-127` free-slot resident leaves.

Broad direct-write experiments for all `32-127` below-tail leaves regressed the
prepared insert path, so this slice keeps the publication rule bounded to an
equal-capacity pressure-victim case rather than widening by free-slot band
alone.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage behavior only in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source is involved.
- `merge_dirty_page_buffer()` asks
  `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` whether a
  child dirty-buffer page should publish directly or replay into the parent
  dirty buffer.
- `dirty_page_buffer_pressure_flush_index()` identifies the resident page that
  fallback replay would flush when the parent dirty buffer is full.
- Existing full, near-full, `16-31`, replaced-broad-victim, and dense-victim
  predicates already protect the high-confidence cases. Residual rejected
  candidates are visible through the prepared-insert benchmark's pressure-victim
  matrix.

## Design

Add a direct-write guard outcome:
`future-current-header-equal-broad-victim-direct-write`.

The outcome applies only after the existing future-current guards have passed
and all of the following are true:

- the incoming page is an index leaf with `64-127` free slots;
- the incoming page is `32-127` pages below the parent dirty-buffer leaf tail;
- the parent dirty buffer is full;
- the pressure victim is checksum-dirty; and
- the pressure victim is an index leaf with `64-127` free slots.

When the outcome fires, the incoming child dirty-buffer page is direct-written
through the existing merge publication path, and the equal-capacity resident
victim remains buffered. The existing replaced-broad-victim lifecycle counters
remain scoped to already-replaced resident victims; the new behavior is reported
through the guard outcome tables and residual rejected-candidate summaries.

## Compatibility Impact

No SQL behavior, public MyLite C API behavior, handler API behavior, metadata,
storage-engine routing, or file-format behavior changes. `ENGINE=InnoDB`
continues to route through MyLite. The change only affects internal dirty index
leaf publication order during nested dirty-buffer merge pressure.

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

No new dependencies. Production builds add one small guard branch and fixed leaf
free-slot metadata checks in the dirty-buffer merge path.

## Tests And Verification

- Add focused storage self-test coverage proving the new guard:
  - direct-writes a `64-127` free-slot incoming below-tail leaf;
  - preserves a `64-127` free-slot resident victim;
  - does not increment the replaced-broad-victim matrix; and
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

- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `398.53 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `337.51 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with `33,979,890` byte (`32.41 MiB`) `libmariadbd.a`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed. The prepared insert step was `76.568 us/op`.

The prepared-insert profile reported:

- `2,113` dirty `index-leaf`
  `future-current-header-equal-broad-victim-direct-write` rows;
- `26,199` dirty `index-leaf` buffer-limit pressure admissions, down from
  `28,551` in the dense-broad-victim profile;
- `61,711` dirty `index-leaf` merge direct writes, up from `59,392`;
- `87,911` index-leaf dirty refreshes and `234,647` zero-tail checksum calls,
  down from `87,944` and `234,680`;
- residual rejected below-tail candidate admissions of `4,461`, down from
  `6,634`; and
- maintained-root decode sites unchanged at `677` total, concentrated in
  planning and journal validation.

## Acceptance Criteria

- The new guard fires only for `64-127` incoming leaves that would evict a
  checksum-dirty `64-127` resident victim.
- Existing full, near-full, `16-31`, replaced-broad-victim, dense-victim,
  fallback, and rollback behavior remains unchanged outside that predicate.
- Prepared-insert benchmark evidence shows lower dirty leaf pressure without a
  broad-direct-write regression.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Equal broad victims are a heuristic, not a semantic hotness proof. Benchmark
  evidence must remain the gate.
- Page-id tail distance is an append-workload proxy, not a durable index-edge
  invariant.
