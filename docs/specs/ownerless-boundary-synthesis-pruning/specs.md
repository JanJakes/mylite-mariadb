# Ownerless Boundary Synthesis Pruning

## Problem

The ownerless write-performance probe shows the autocommit write path still
spends time in page-version publication before appending the real page record.
The previous native boundary synthesis slice made active-reader cleanup more
precise, but the hook now checks for snapshot-boundary work on every published
page, including the normal no-active-pin write loop and InnoDB native-support
page classes that the page-log checkpoint logic already treats as not needing
an oldest-snapshot boundary.

This slice removes that unnecessary work without changing page-version WAL
append ordering, native-support history proof publication, or recovery
semantics. It is a bounded hot-path cleanup, not the larger ownerless
history-proof redesign that remains the main write-throughput target.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_publish()` publishes dirty in-file pages at MTR
  commit boundaries and classifies InnoDB page types before deciding whether a
  native-support page can be elided.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_page_publish_hook()` calls
  `publish_ownerless_snapshot_boundary_if_needed()` before appending each page
  version. The boundary helper snapshots
  `ownerless_page_pin_registry` and can read an older native page image when an
  active pin has an older read LSN than the commit-visible LSN.
- `packages/libmylite/src/ownerless_page_pin_registry.cc`
  `mylite_ownerless_page_pin_registry_active_count()` reads the shared active
  count without taking the registry latch. The existing single-owner refresh
  skip path already uses this as a fast proof that no page-version pins are
  currently active.
- `packages/libmylite/src/ownerless_page_log.cc`
  `record_requires_oldest_snapshot_boundary()` returns false for InnoDB
  native-support state pages: allocated, undo-log, inode, insert-buffer,
  system, transaction-system, FSP header, and extent descriptor pages. It
  remains conservative for unknown and user data/index/blob page classes.

## Design

Page-version publication now prunes boundary synthesis in two cases:

- If the published page image is an InnoDB native-support state page, the hook
  appends the actual page-version record directly. These records are still
  published when needed for the ownerless native/history proof, but they do not
  trigger synthesized oldest-snapshot boundary probes.
- If no page-version pins are active, the boundary helper returns before taking
  the page-pin registry latch or probing the page-version WAL/native
  tablespace. The later latched snapshot remains authoritative when the shared
  active count is nonzero.

The page image classifier uses the same FIL page-type constants already used by
MyLite native checkpoint proof code and by the ownerless page-log retention
classifier. Unknown, data/index, BLOB, compressed BLOB, and other
snapshot-sensitive pages still run through the existing boundary synthesis
path when active pins exist.

Boundary synthesis remains opportunistic. A pin that starts after the hook has
already checked for active pins could already miss synthesis in the previous
latched path if it started after that snapshot. In both designs, failure to
synthesize a boundary does not fail commit; WAL retention remains
conservative until the active pin releases.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, or directory-layout behavior changes.
Transaction isolation and active-reader visibility are unchanged: the slice
only skips auxiliary boundary work for page classes or registry states that do
not require a synthesized oldest-snapshot boundary record.

## Database Directory And Lifecycle Impact

No files or directory layouts are added. The existing
`concurrency/mylite-concurrency.wal`, page-version index, page-pin registry,
and checkpoint records keep their current format and lifecycle. Skipped
boundary probes do not alter normal page-version appends.

## Native Storage Impact

Native InnoDB files and page formats are unchanged. Native-support proof pages
remain normal ownerless page-version records when the MTR/history-proof path
requires publication. This slice only avoids reading a second older native
page image for those native-support page classes as a synthesized boundary.

## Build And Performance Impact

The hot path adds a small FIL page-type read before the boundary helper and
removes a page-pin registry latch/snapshot plus later WAL/native-page probes in
the common no-active-pin case. Stats-enabled production probes should show
lower `*_page_publish_hook_boundary_ms` while page-version append counts,
native-support history-proof counts, and actual boundary append counts remain
semantically unchanged for their workloads.

This does not address the larger measured cost from publishing full
history-proof native-support pages or appending three page-version records per
simple ownerless autocommit insert.

## Test And Verification Plan

- Build the production embedded performance probe and ownerless SQL test.
- Run a reduced stats-enabled production performance probe and compare
  boundary-hook elapsed time and page-version counts against the pre-slice
  profile.
- Run focused ownerless active-pin boundary SQL coverage to prove active
  snapshots still retain WAL until release and final state survives reopen and
  forced shared-memory rebuild.
- Run the ownerless native-support/history-proof focused selector to prove the
  native-support proof pages are still published or elided under the existing
  rules.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- No-active-pin ownerless autocommit writes skip the latched boundary snapshot
  and reduce measured boundary-hook time.
- Native-support page publications no longer invoke synthesized boundary
  probing, while their normal page-version publication and history-proof
  attribution remain intact.
- Active-reader boundary synthesis remains available for snapshot-sensitive
  pages and focused SQL active-pin coverage still passes.
- Docs and compatibility notes state that this is a bounded pruning slice, not
  completion of the ownerless write-throughput objective.

## Verification Results

Local verification on 2026-06-12 used production artifacts:
`build/php-embedded-prod` was `Release` and `build/mariadb-embedded` was
`MinSizeRel`.

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed after formatting.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-pin-reclaim-boundary` passed.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision)$'
  --output-on-failure` passed both focused hook tests.
- A reduced stats-enabled production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. The post-slice run
  reported `page_publish_hook_boundary_ms=0.035`,
  `page_publish_hook_boundary_appends=0`, `3.000` page versions per insert,
  `2.000` native-support published pages per insert, `1.000` native-support
  elided page per insert, and `0.000` actual snapshot-boundary pages per insert.
  The same shaped pre-slice run reported `page_publish_hook_boundary_ms=1.814`
  with zero boundary appends, so the no-active-pin registry/probe work was
  removed while append volume stayed unchanged.
- `ctest --preset ownerless-stress -R
  'active-reader|active_pin|active-pin|boundary|native-support'
  --output-on-failure` passed the native-support elision selector, active-reader
  pressure stress, and active-reader pressure trace generator.
- `tools/check-ci-production-builds`, `tools/require-cmake-release-build
  build/php-embedded-prod`, `tools/require-cmake-build-type MinSizeRel
  build/mariadb-embedded`, `format-check-prod`, and `git diff --check` passed.

## Risks And Follow-Up

- The lock-free active-count fast path is intentionally only a fast negative
  check. The existing latched snapshot still decides the oldest pin when any
  active pin is observed.
- This slice will not materially close the ownerless autocommit throughput gap
  by itself. The next high-impact performance work remains replacing or
  shrinking the history-proof native-support page publication while preserving
  redo/checkpoint and active-reader recovery evidence.
