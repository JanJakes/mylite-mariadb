# Ownerless Page Publish Type Probe

## Problem

Production probes now run against optimized `php-embedded-prod` binaries and
show ownerless autocommit inserts still far slower than ordinary autocommit
inserts. A 400-row production probe reported:

- ordinary autocommit inserts around `2219 ops/s`;
- ownerless autocommit inserts around `148 ops/s`;
- `page_publish_published=3203`;
- `page_log_append_total_ms=167.214`;
- `page_write_publish_total_ms=229.542`;
- `prepared_step_mysql_execute_ms=2254.460`.

The page-log append path is visible but is not the whole slowdown. The next
page-publication reduction must know whether append volume is mostly
snapshot-boundary user/index pages, native-support system/undo/allocation
pages, or another page class. The existing publish counters only counted total
candidates and failures.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_publish()` copies the committed page image,
  verifies that `FIL_PAGE_LSN` matches the mini-transaction commit LSN, then
  calls `mylite_ownerless_innodb_publish_page_version()`.
- `mariadb/storage/innobase/include/fil0fil.h` defines `FIL_PAGE_TYPE` and
  `fil_page_type_is_index()`.
- `packages/libmylite/src/ownerless_page_log.cc` treats allocation, undo,
  inode, insert-buffer, system, transaction-system, FSP header, and XDES pages
  as native-support states for snapshot-boundary retention. Other page types
  still require page-version boundary evidence.
- `packages/libmylite/tests/embedded_performance_probe.c` already emits
  production page-publish and page-log append counters when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Design

Extend the internal InnoDB page-publish stats with append-attempt page classes:

- index pages;
- undo pages;
- space metadata pages;
- transaction/system pages;
- BLOB pages;
- other pages;
- native-support pages;
- snapshot-boundary pages.

The counters are incremented only after the copied page image proves the commit
LSN and immediately before the page-version append hook. Early skipped
candidates are still tracked by the existing skip counters and do not pollute
page-type totals.

## Compatibility Impact

No SQL, C API, PHP API, storage-format, wire-protocol, or page-log format
changes. The new counters are internal diagnostics exposed only through the
existing first-party performance probe declarations.

## Directory And Lifecycle Impact

No durable file or directory-layout change. The counters read the page type from
the already copied InnoDB page image and do not alter checkpoint, page-log, or
process-registry state.

## Native Storage Impact

No native InnoDB behavior changes. The counters preserve MariaDB commit and
page flush behavior.

## Build And Performance Impact

The slice adds relaxed atomic increments only while page-publish stats are
explicitly enabled. Normal ownerless execution and CI test runs with stats off
do not pay the classification cost. CI timing jobs continue to use production
build presets.

## Test Plan

- Rebuild the production embedded targets that include `mtr0mtr.cc` and the
  performance probe.
- Run the production performance probe with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and confirm the new page-type
  counters are emitted.
- Run focused ownerless primitive and SQL selectors covering page publication,
  live reclaim, active-reader pressure, and commit-race behavior.
- Run production `ownerless-stress`, production unsafe-hook negative-proof
  coverage, `format-check-prod`, and `git diff --check`.

## Verification Evidence

A reduced 400-row production probe after this slice classified ownerless
autocommit page-version publication as:

- `page_publish_published=3203`;
- `page_publish_type_index=400`;
- `page_publish_type_undo=1200`;
- `page_publish_type_space_metadata=803`;
- `page_publish_type_trx_system=800`;
- `page_publish_native_support=2803`;
- `page_publish_snapshot_boundary=400`.

The sample shows most append volume is native-support page state, not user
index page boundaries. That points the next optimization toward native-support
publish reduction, but only after redo/checkpoint reconciliation proves those
appends can be skipped or compacted safely.

## Acceptance Criteria

- Page-type counters are emitted by the production performance probe.
- Existing publish, page-log, and ownerless correctness tests keep passing.
- Docs mark the result as diagnostic evidence, not a completed optimization.

## Risks And Follow-Up

- The counters classify page images; they do not reduce append volume.
- If the probe shows most writes are snapshot-boundary index/user pages, the
  next reduction likely needs per-statement/page batching or dirty-page handoff.
- If the probe shows mostly native-support pages, the next reduction likely
  needs broader redo/checkpoint reconciliation before safely skipping appends.
