# Ownerless Insert FK Fast-Path Cache

## Problem Statement

The ownerless FK crash fast-path fix correctly disables direct page-image
visibility for literal `INSERT ... VALUES` statements whose target table owns
foreign-key constraints. A refreshed production performance probe after that
slice showed the guard had become a hot-path cost for non-FK insert workloads:
each prepared autocommit step and each direct bulk insert statement queried
`information_schema.referential_constraints` before deciding that the visible
fast path was still safe.

That preserved correctness, but it made stable non-FK ownerless insert loops
pay metadata I/O that should be amortized until the dictionary changes.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_statement_allows_visible_fast_path()` runs inside the ownerless
  direct and prepared execution paths after statement locks and dictionary
  refresh have completed.
- `ownerless_insert_target_has_foreign_keys()` resolves the normalized target
  schema/table and reads `information_schema.referential_constraints`.
- Ownerless dictionary state already exposes a generation counter through
  `mylite_ownerless_dictionary_state_wait_ready()`. MyLite records the observed
  generation in `mylite_db::ownerless_observed_dictionary_generation` and
  updates it on cross-process dictionary refresh and local DDL begin/finish.
- `packages/libmylite/tests/embedded_performance_probe.c` exercises the hot
  paths:
  - `measure_autocommit_insert()` reuses a prepared
    `INSERT INTO app.<table> (id, value) VALUES (?, ?)`;
  - `measure_bulk_autocommit_insert()` emits direct multi-row literal
    `INSERT ... VALUES` statements.

## Design

Add a MyLite-owned per-handle cache for ownerless insert FK decisions:

1. Resolve the insert target schema/table as before.
2. Look for a cache entry matching normalized schema/table and the handle's
   current ownerless dictionary generation.
3. If present, reuse the cached `has_foreign_keys` value.
4. If absent, query `information_schema.referential_constraints`, cache the
   result for the current dictionary generation, and use it for the fast-path
   decision.
5. Treat parse failures or metadata query failures as unsafe and do not rely
   on a cache entry for them.
6. Clear the cache whenever the handle observes a new ownerless dictionary
   generation, including successful cross-process refresh and successful local
   dictionary DDL finish.

The cache is deliberately handle-local. It does not add shared-memory state,
does not become a source of dictionary truth, and does not outlive the open
MyLite handle.

## Compatibility Impact

No SQL syntax, public C API, mysqli/PHP API, storage format, or directory
layout changes. The FK crash behavior remains unchanged: FK-constrained insert
targets still disable visible page-image fast publishing, so rejected orphan
rows do not become visible through ownerless page images.

## Directory And Lifecycle Impact

No durable files or shared-memory fields are added. Cache entries live in the
`mylite_db` heap object and are released on close. Dictionary generation
refresh and local DDL completion invalidate entries before later DML can use
them.

## Native Storage Impact

Native InnoDB remains responsible for FK enforcement and statement rollback.
MyLite only caches the metadata answer used to decide whether its ownerless
pre-success page-image fast path is eligible.

## Build And Performance Impact

The current production stats-off sample with
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2`,
`MYLITE_PERF_SELECT_ITERATIONS=500`, and
`MYLITE_PERF_INSERT_ITERATIONS=1000` reported:

- ownerless prepared autocommit inserts: `517.27 ops/s`;
- ordinary prepared autocommit inserts: `880.24 ops/s`;
- ownerless direct bulk rows: `1441.31 ops/s`;
- ordinary direct bulk rows: `7969.91 ops/s`.

The expected improvement is fewer metadata queries on repeated non-FK insert
targets. This slice does not claim to solve the remaining native ownerless
page-log, commit-MTR, redo/checkpoint, or active-reader costs.

## Test And Verification Plan

- Add ownerless SQL coverage that warms a non-FK insert cache entry, mutates
  the table with `ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY`, then proves
  a rejected direct and prepared orphan insert does not leave a row behind.
- Rebuild and run the focused ownerless visible-fast-path selector.
- Run `dictionary-foreign-key-crash` to preserve the prior FK crash fix.
- Build the production performance probe and rerun a reduced stats-off sample.
- Run format checks, `tools/check-ci-production-builds`, and `git diff --check`.

## Acceptance Criteria

- Repeated non-FK ownerless insert execution avoids repeated FK metadata
  lookups until dictionary generation changes.
- FK-constrained tables still disable ownerless visible fast publishing.
- Dictionary DDL invalidates stale non-FK decisions before later DML.
- The reduced production performance probe recovers the regression caused by
  the per-statement FK metadata query.

## Verification Results

Local verification on 2026-06-13 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel` and `build/php-embedded-prod` was
`Release`.

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test`: passed.
- Direct ownerless selectors passed:
  `single-owner-multi-row-insert-visible-fast-path`,
  `insert-fk-fast-path-cache`, and `dictionary-foreign-key-crash`.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.(ownerless-(single-owner-multi-row-insert-visible-fast-path|insert-fk-fast-path-cache|primitives)|embedded-ownerless-innodb-lock-hooks)$'
  --output-on-failure`: passed, 4/4 tests.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`:
  passed.
- A reduced stats-off production performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2`,
  `MYLITE_PERF_SELECT_ITERATIONS=500`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` passed after the cache. It reported
  ownerless prepared autocommit inserts at `1294.06 ops/s` and ownerless
  direct bulk rows at `3705.93 ops/s`, recovering most of the immediate
  regression from the pre-cache sample (`517.27 ops/s` and `1441.31 ops/s`,
  respectively). Ordinary prepared autocommit inserts in the same post-cache
  sample reported `3223.41 ops/s`, and ordinary direct bulk rows reported
  `11740.40 ops/s`.
- `cmake --build --preset ownerless-test-hooks --target format-check`:
  passed.
- `cmake --build --preset format-check-prod`: passed after refreshing the
  local `build/prod` cache to use the available
  `/home/agent/.local/bin/clang-format`.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` and
  `tools/require-cmake-release-build build/php-embedded-prod build/prod`:
  passed.
- `git diff --check`: passed.

## Risks And Follow-Up

- The cache uses dictionary generation as the invalidation proof. If future
  ownerless metadata changes bypass dictionary state, they must either refresh
  the generation or explicitly clear this cache.
- Remaining ownerless insert overhead after this slice belongs to native
  page-version publication, redo/checkpoint proof, commit visibility, and
  active-reader policy rather than FK metadata lookup.
