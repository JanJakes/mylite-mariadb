# Ownerless Page Publish Classification

## Problem

Production ownerless attribution logs use the MTR page-publish counter named
`snapshot_boundary` as if it were the number of synthesized active-reader
boundary records. In the current MariaDB-side implementation, that counter is
actually the complement of `native_support`: ordinary index/data pages are
counted there too. This makes performance triage ambiguous when deciding
whether ownerless autocommit cost comes from normal user-page publication or
from active snapshot boundary synthesis.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_publish_count_page_type()` increments
  `ownerless_page_publish_native_support` for explicit native-support page
  classes and increments `ownerless_page_publish_snapshot_boundary` for every
  other publishable page class, including ordinary index pages.
- `packages/libmylite/src/database.cc`
  `publish_ownerless_snapshot_boundary_if_needed()` is the actual synthesized
  boundary path. It appends an older native page image only when an active
  page-version pin exists and the native page proves a boundary at or before
  the oldest pinned LSN.
- `packages/libmylite/tests/embedded_performance_probe.c` derives
  per-insert ownerless autocommit summaries from both MTR page-publish stats
  and first-party database hook stats.

## Design

Keep existing raw and summary keys so older log scrapers continue to work.
Add clearer diagnostic keys:

- `*_page_publish_non_native_support` as an alias for the existing MTR
  non-native-support complement counter;
- `mylite_perf_summary_ownerless_autocommit_non_native_support_pages_per_insert`;
- `*_page_publish_hook_boundary_appends`, counted only after
  `publish_ownerless_snapshot_boundary_if_needed()` successfully appends an
  actual synthesized boundary record;
- `mylite_perf_summary_ownerless_autocommit_actual_snapshot_boundary_pages_per_insert`.

This is a measurement change only. It does not alter page-version WAL records,
index publication, active-pin policy, or native page reads.

## Compatibility Impact

No SQL, C API, PHP API, storage-engine, wire-protocol, or directory-layout
behavior changes. The new keys clarify internal performance evidence for CI
and local production probes.

## Native Storage Impact

No native InnoDB format or redo/checkpoint changes. The actual boundary counter
observes successful MyLite page-version WAL appends from the existing
best-effort native boundary synthesis path.

## Build And Performance Impact

Default runtime behavior is unchanged. When ownerless database performance
stats are enabled, a successful synthesized boundary append increments one
process-local atomic counter. The production stats-off performance probe is
unchanged.

## Test Plan

- Build the production embedded performance probe.
- Run a reduced stats-enabled production probe and verify the new raw and
  summary keys are present.
- Run focused ownerless visibility coverage.
- Run production build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Existing `snapshot_boundary` keys remain present.
- New non-native-support keys expose the existing MTR complement meaning.
- New actual snapshot-boundary keys remain zero for an unpinned ownerless
  autocommit insert loop and become the authority for future boundary synthesis
  attribution.
- Focused ownerless correctness coverage continues to pass.

## Verification Results

Local verification on 2026-06-10 used production artifacts:
`build/php-embedded-prod` was `Release` and `build/mariadb-embedded` was
`MinSizeRel`.

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- A reduced stats-enabled production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. The ownerless
  autocommit loop reported `300` published page-version records, `357`
  native-support page candidates, `157` native-support elisions, `100`
  non-native-support page candidates, the legacy `snapshot_boundary=100`, and
  `0` actual synthesized boundary appends. The summary keys reported
  `3.000` page-version records per insert, `1.000` non-native-support page per
  insert, and `0.000` actual snapshot-boundary pages per insert. The same run
  reported ownerless autocommit at `928.62 ops/s` versus ordinary autocommit at
  `1756.67 ops/s`.
- Focused production ownerless selectors passed:
  `prepared-committed-read` and `active-pin-reclaim-boundary`.

## Risks And Follow-Up

- The legacy `snapshot_boundary` key remains for compatibility with existing
  logs, but new analysis should prefer `non_native_support` for MTR page-class
  volume and `actual_snapshot_boundary` for active-reader boundary synthesis.
- This slice does not reduce page-version write volume. It makes the remaining
  page-publication optimization target clearer.
