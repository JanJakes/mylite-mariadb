# Bounded Branch Refold Page Budget

## Problem

The 100k prepared-insert component run is now dominated by append write volume,
not cache lookup. A kept benchmark file showed `290895` pages, with page types:

- index leaf pages: `217180`
- append-only index-entry pages: `65639`
- row pages: `8058`

The current level-`1` branch refold fallback can rewrite a fresh branch snapshot
with hundreds of leaf pages to absorb a single append-tail overlay entry. That
keeps the static branch clean, but it is a poor write-amplification tradeoff for
large branches and duplicate-heavy prepared insert workloads.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches MyLite through:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()`
  - `mariadb/sql/handler.cc:handler::ha_write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()`
  - `mariadb/storage/mylite/ha_mylite.cc` calls
    `mylite_storage_append_row_with_index_entries()` for durable routed tables.
- MyLite storage refs:
  - `plan_branch_index_root_insert()` uses refold fallback after local split
    and bounded redistribution paths miss.
  - `build_branch_index_refold_insert_entryset_if_fit()` already prechecks
    whether the post-insert entry count can fit under one level-`1` branch.
  - `refold_branch_index_root_insert()` writes a complete fresh leaf run plus
    the branch root.
  - The existing fallback append-tail index-entry path remains correctness
    preserving when a maintained branch plan is not chosen.

## Design

Add a fixed page budget for whole-branch refold insert planning:

1. Compute the required post-insert leaf page count from the branch entry count
   and fixed key leaf capacity before copying or rebuilding the refold entryset.
2. Keep the existing branch-child-capacity precheck.
3. Return `fits = false` when the required leaf count exceeds the refold page
   budget.
4. Leave smaller refolds unchanged.

The budget is intentionally conservative. Large live-overlay refolds remain
correct through append-tail index-entry fallback, and later roadmap work can
replace that fallback with localized level-`2`/deeper live-overlay maintenance.

## Compatibility Impact

No SQL, public API, handler, storage-engine routing, or file-format behavior
changes. The affected insert remains visible through the existing append-tail
overlay path when a large refold is skipped.

## Single-File And Embedded Lifecycle

No new companion files or lifecycle states. Durable state remains in the
primary `.mylite` file. The change reduces large transient refold writes and
can leave more append-tail index-entry pages for later maintenance.

## Binary-Size, License, And Dependency Impact

No dependency, license, or embedded profile changes. Binary-size impact is a
single bounded planning check and one focused test hook.

## Test And Verification Plan

- Add storage hook coverage proving an over-budget level-`1` refold candidate
  returns no fit without reading or copying a refold entryset.
- Keep the existing over-capacity refold precheck coverage passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Over-budget branch refolds skip entryset reads/copies and use append-tail
  fallback instead.
- Smaller refold candidates still use the existing behavior.
- Storage tests pass.
- The prepared-insert 100k run records reduced index-leaf write volume or
  identifies the next bottleneck.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `150.82 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `231.31 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed with prepared insert step at `25.920 us/op`, commit at
  `121.790 ms`, and final page count `73177`.

The kept benchmark file confirmed the intended write-volume shift:

- index leaf pages dropped from the prior local `217180` to `409`;
- total page count dropped from `290895` to `73177`;
- append-only index-entry pages remained high at `65663`, making append-tail
  overlay cleanup the next measured bottleneck.

## Risks

- Skipping large refolds can increase append-tail overlay length and hurt later
  exact-index reads until localized overlay maintenance lands.
- Setting the budget too low would choose append-tail fallback where a small
  refold would be cheaper. The initial cap is high enough to keep small root
  cleanup behavior while avoiding hundreds-of-page snapshots.
