# Ownerless Native Support Publish Attribution

## Problem

Production ownerless autocommit probes now distinguish ordinary non-native
page publication from actual active-reader snapshot-boundary synthesis, but
the native-support page bucket remains too coarse. A reduced production probe
reported `3.570` native-support page candidates per insert and `1.570`
native-support elisions per insert, while the remaining published page-version
records still dominate the ownerless autocommit optimization target. The next
safe optimization needs to know which native-support page classes still
publish and which classes are already elided.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_publish()` copies a committed page image,
  verifies that `FIL_PAGE_LSN` matches the mini-transaction commit LSN, and
  either elides an eligible native-support page or calls
  `mylite_ownerless_innodb_publish_page_version()`.
- `ownerless_page_publish_type_has_native_support()` treats undo-log pages,
  InnoDB space metadata pages, system pages, and transaction-system pages as
  native-support states.
- `ownerless_page_write_can_elide_native_support_page()` currently allows
  native-support elision only for ownerless autocommit statements whose page is
  covered by the transaction rollback-segment space proof and not by an active
  history-proof page requirement.
- `packages/libmylite/tests/embedded_performance_probe.c` mirrors the
  MariaDB-side page-publish counter order and prints the raw counters and
  ownerless autocommit summary keys when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  a focused `single-owner-native-support-page-wal-elision` selector that
  asserts native-support elision is active and that no publication failures or
  early skips occurred.

## Design

Append new MariaDB-side page-publish counters without changing existing
counter indexes:

- total native-support pages that successfully published;
- successfully published native-support undo pages;
- successfully published native-support space-metadata pages;
- successfully published native-support transaction-system pages;
- elided native-support undo pages;
- elided native-support space-metadata pages;
- elided native-support transaction-system pages.

The counters are relaxed atomics gated by the existing opt-in
`ownerless_page_publish_stats_enabled` flag. Published counters increment only
after the page-version hook returns `MYLITE_OWNERLESS_INNODB_LOCK_OK`. Elided
counters increment only in the existing native-support elision branch after
the page type has already been counted in the legacy total bucket.

The performance probe emits raw keys and per-insert summary keys for the new
breakdown. The focused native-support elision SQL selector checks that the
published and elided type buckets add up to their corresponding totals when no
publication failures occurred.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, wire-protocol, storage-engine, storage-format,
or directory-lifecycle behavior changes. The slice changes diagnostics only.
The existing raw page-publish counter indexes and keys remain present; the new
counters are appended.

## Directory And Lifecycle Impact

No durable files, shared-memory layout, page-version WAL records, checkpoint
records, native tablespaces, or cleanup semantics change.

## Native Storage Impact

No native InnoDB redo, undo, page, or checkpoint behavior changes. The
counters observe already-classified page images on the existing publish and
elision paths.

## Build And Performance Impact

Default stats-off ownerless execution is unchanged. Stats-enabled probe runs
pay a few additional relaxed atomic increments per relevant native-support
page. Because `mtr0mtr.cc` is part of the bundled MariaDB embedded archive,
the production `MinSizeRel` MariaDB embedded archive must be rebuilt before
verification.

## Test And Verification Plan

- Verify production build guards for `build/mariadb-embedded` and
  `build/php-embedded-prod`.
- Rebuild the MariaDB embedded archive with `tools/mariadb-embedded-build
  build`.
- Build production `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test`.
- Run the focused production SQL selector
  `single-owner-native-support-page-wal-elision`.
- Run a reduced stats-enabled production performance probe and confirm the new
  raw and summary keys are present.
- Run focused ownerless visibility/reclaim coverage, production build guards,
  `format-check-prod`, `tools/check-ci-production-builds`, and
  `git diff --check`.

## Acceptance Criteria

- Existing raw page-publish keys and counter indexes remain compatible.
- The production performance probe emits published and elided native-support
  page-class counters.
- The focused SQL selector proves the new native-support accounting adds up in
  a no-failure ownerless insert loop.
- Docs keep the result as performance attribution evidence rather than a new
  native-support publication optimization.

## Verification Results

Local verification on 2026-06-10 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel` and `build/php-embedded-prod` was
`Release`.

- `tools/mariadb-embedded-build build` rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- Direct focused selectors passed for
  `single-owner-page-write-refresh-skip`,
  `single-owner-external-refresh-skip`,
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`,
  `single-owner-skip-peer-history`,
  `prepared-committed-read`, `active-pin-reclaim-boundary`, and
  `commit-race`.
- The production CTest subset passed:
  `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-(primitives|single-owner-(page-write-refresh-skip|external-refresh-skip|history-wal-proof|native-support-page-wal-elision)|uncommitted-peer-hidden)$'
  --output-on-failure`.
- A reduced stats-enabled production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. The ownerless
  autocommit loop reported `457` page-publish candidates, `300` published
  page-version records, `357` native-support page candidates, `157`
  native-support elisions, `200` published native-support pages, `100`
  non-native-support pages, zero publish failures, and zero actual synthesized
  snapshot-boundary appends. The published native-support split was `100`
  undo pages, `0` space-metadata pages, and `100` transaction-system pages.
  The elided native-support split was `100` undo pages, `38` space-metadata
  pages, and `19` transaction-system pages. Summary keys reported `3.000`
  page-version records per insert, `3.570` native-support candidates per
  insert, `1.570` native-support elisions per insert, `2.000` published
  native-support pages per insert, `1.000` published undo page per insert,
  `1.000` published transaction-system page per insert, `0.380` elided
  space-metadata pages per insert, and `1.000` non-native-support page per
  insert. The same stats-enabled sample reported ownerless autocommit at
  `377.61 ops/s` versus ordinary autocommit at `1582.10 ops/s`; this run is
  diagnostic because page-publish stats are enabled.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed,
  `tools/require-cmake-release-build build/ownerless-test-hooks` passed, and
  `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure` passed all 3 hook negative-proof tests.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed. The full
  `ctest --preset ownerless-stress --output-on-failure` run passed
  independent-table stress, DDL stress, temporary-table stress, and
  transaction stress, then timed out in `ownerless-cross-process-checksum-stress`
  at `900.09s` with an InnoDB fatal semaphore wait on `dict_sys.latch`.
  A direct isolated `checksum-stress` rerun and a reduced
  `MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=1` rerun also stalled and were
  terminated. The failure occurs with page-publish stats disabled and is
  recorded as separate checksum-stress debt rather than evidence that the new
  stats-only counters changed ownerless behavior.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `cmake --build --preset format-check-prod`, `git diff --check`,
  `tools/check-ci-production-builds`, and
  `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.

## Risks And Follow-Up

- The counters identify the remaining native-support publication shape; they
  do not reduce page-version WAL volume by themselves.
- The final production attribution sample shows the remaining published
  native-support pages on this hot path are undo and transaction-system pages,
  not space metadata. The next optimization likely needs a stronger native
  redo/checkpoint proof for those history-related pages instead of broader
  space-metadata elision.
- If published native-support pages include broader space metadata or
  transaction-system churn, the next optimization must first prove recovery and
  active-reader behavior across those page classes.
