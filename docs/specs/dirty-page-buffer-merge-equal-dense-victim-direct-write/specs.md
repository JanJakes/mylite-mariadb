# Dirty-Page Buffer Merge Equal Dense Victim Direct Write

## Problem

After the equal broad-victim direct-write slice, the prepared-insert profile
still reports `26,199` dirty `index-leaf` buffer-limit pressure admissions and
`87,911` index-leaf dirty refreshes. The residual below-tail pressure-victim
matrix now leaves a narrow high-count class: `32-63` free-slot incoming
future-current leaves still evict checksum-dirty `32-63` free-slot resident
leaves.

Earlier broad direct-write experiments for all `32-127` below-tail leaves
regressed the prepared insert path, so this slice keeps the publication rule
bounded to the equal dense pressure-victim case.

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
- Existing full, near-full, `16-31`, replaced-broad-victim, dense-broad-victim,
  and equal-broad-victim predicates already cover higher-confidence cases.
  Residual rejected candidates are visible through the prepared-insert
  benchmark's pressure-victim matrix.

## Design

Add a direct-write guard outcome:
`future-current-header-equal-dense-victim-direct-write`.

The outcome applies only after the existing future-current guards have passed
and all of the following are true:

- the incoming page is an index leaf with `32-63` free slots;
- the incoming page is `32-127` pages below the parent dirty-buffer leaf tail;
- the parent dirty buffer is full;
- the pressure victim is checksum-dirty; and
- the pressure victim is an index leaf with `32-63` free slots.

When the outcome fires, the incoming child dirty-buffer page is direct-written
through the existing merge publication path, and the equal-density resident
victim remains buffered. The existing replaced-broad-victim lifecycle counters
remain scoped to already-replaced resident victims; the new behavior is
reported through the guard outcome tables and residual rejected-candidate
summaries.

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
  - direct-writes a `32-63` free-slot incoming below-tail leaf;
  - preserves a `32-63` free-slot resident victim;
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
- `build/dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `418.85 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `338.66 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with `33,980,162` byte (`32.41 MiB`) `libmariadbd.a`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed. The prepared insert step was `76.368 us/op`.

The prepared-insert profile reported:

- `2,542` dirty `index-leaf`
  `future-current-header-equal-dense-victim-direct-write` rows;
- `22,733` dirty `index-leaf` buffer-limit pressure admissions, down from
  `26,199` in the equal-broad-victim profile;
- `64,611` dirty `index-leaf` merge direct writes, up from `61,711`;
- `87,345` index-leaf dirty refreshes and `234,081` zero-tail checksum calls,
  down from `87,911` and `234,647`;
- residual rejected below-tail candidate admissions of `1,606`, down from
  `4,461`; and
- maintained-root decode sites unchanged at `677` total, concentrated in
  planning and journal validation.

## Acceptance Criteria

- The new guard fires only for `32-63` incoming leaves that would evict a
  checksum-dirty `32-63` resident victim.
- Existing full, near-full, `16-31`, replaced-broad-victim,
  dense-broad-victim, equal-broad-victim, fallback, and rollback behavior
  remains unchanged outside that predicate.
- Prepared-insert benchmark evidence shows lower dirty leaf pressure without a
  broad-direct-write regression.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Equal dense victims are a heuristic, not a semantic hotness proof. Benchmark
  evidence must remain the gate.
- Page-id tail distance is an append-workload proxy, not a durable index-edge
  invariant.
