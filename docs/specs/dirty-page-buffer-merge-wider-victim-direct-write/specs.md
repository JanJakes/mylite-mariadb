# Dirty-Page Buffer Merge Wider Victim Direct Write

## Problem

After the equal dense-victim direct-write slice, the prepared-insert profile
still reports `22,733` dirty `index-leaf` buffer-limit pressure admissions and
`87,345` index-leaf dirty refreshes. The residual rejected below-tail candidate
summary is now narrow: `1,606` future-current partial leaves with `32-127` free
slots sit `32-127` pages below the parent dirty-buffer leaf tail.

The remaining pressure-victim matrix shows most of those candidates would evict
a checksum-dirty leaf with a wider free-slot band: `32-63` incoming leaves
mostly evict `64-127` or `128+` free-slot victims, and `64-127` incoming leaves
mostly evict `128+` free-slot victims. Earlier broad direct-write experiments
regressed the path, so this slice tests only the strictly wider-victim residual
case instead of direct-writing every broad below-tail candidate.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage behavior only in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  source is involved.
- `merge_dirty_page_buffer()` computes a merge direct-write guard outcome
  before either publishing the child entry directly or replaying it into the
  parent dirty buffer.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
  limits future-current direct writes to pages within the parent current
  header, outside parent/child append buffers, and not already resident in the
  parent dirty buffer.
- `dirty_page_buffer_pressure_flush_index()` identifies the resident page that
  fallback replay would flush when the parent dirty buffer is full.
- Existing full, near-full, `16-31`, replaced-broad-victim,
  dense-broad-victim, equal-broad-victim, and equal-dense-victim predicates
  cover higher-confidence cases. Residual rejected candidates are visible
  through the prepared-insert benchmark's pressure-victim matrices.

## Design

Add a direct-write guard outcome:
`future-current-header-wider-victim-direct-write`.

The outcome applies only after the existing future-current guards have passed
and all of the following are true:

- the incoming page is an index leaf with `32-63` or `64-127` free slots;
- the incoming page is `32-127` pages below the parent dirty-buffer leaf tail;
- the parent dirty buffer is full;
- the pressure victim is checksum-dirty; and
- the pressure victim is an index leaf with a strictly wider free-slot band
  than the incoming leaf: `64-127` or `128+` behind `32-63`, or `128+` behind
  `64-127`.

When the outcome fires, the incoming child dirty-buffer page is direct-written
through the existing merge publication path, and the wider-capacity resident
victim remains buffered. The existing replaced-broad-victim lifecycle counters
remain scoped to already-replaced broad victims; the new behavior is reported
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
  - direct-writes a `32-63` free-slot incoming below-tail leaf;
  - preserves a `64-127` free-slot resident victim;
  - does not increment the replaced-broad-victim matrix; and
  - keeps rollback truncation protection for the future-current page.
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

## Verification Evidence

- `cmake --build --preset dev --target mylite_storage_test` passed.
- `build/dev/packages/mylite-storage/mylite_storage_test` passed.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `326.45 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed with `mylite-storage.capabilities` in `317.14 sec` and
  `libmylite.embedded-storage-engine` in `15.17 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with `33,981,066` byte (`32.41 MiB`) `libmariadbd.a`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  passed. The prepared insert step sampled `105.892 us/op` while the host was
  noisy, so wall-clock timing is treated as structural-only evidence.

The prepared-insert profile reported:

- `2,119` dirty `index-leaf`
  `future-current-header-wider-victim-direct-write` rows;
- `21,031` dirty `index-leaf` buffer-limit pressure admissions, down from
  `22,733` in the equal dense-victim profile;
- `66,144` dirty `index-leaf` merge direct writes, up from `64,611`;
- `87,176` index-leaf dirty refreshes and `227,063` zero-tail checksum calls,
  down from `87,345` and `227,232`;
- residual rejected below-tail candidate admissions of `121`, down from
  `1,606`; and
- maintained-root decode sites unchanged at `677` total, concentrated in
  planning and journal validation.

## Acceptance Criteria

- The new guard fires only for the strictly wider-victim predicate.
- Existing full, near-full, `16-31`, replaced-broad-victim,
  dense-broad-victim, equal-broad-victim, equal-dense-victim, fallback, and
  rollback behavior remains unchanged outside that predicate.
- Prepared-insert benchmark evidence shows lower dirty leaf pressure without a
  broad-direct-write regression.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Wider victims are a heuristic, not a semantic hotness proof. Benchmark
  evidence must remain the gate.
- Page-id tail distance is an append-workload proxy, not a durable index-edge
  invariant.
