# Ownerless History Proof Page Attribution

## Problem

The ownerless SYS page identity proof showed that the remaining published
`FIL_PAGE_TYPE_SYS` records in a reduced ownerless autocommit insert sample are
undo-tablespace pages. The sample also still publishes one `FIL_PAGE_UNDO_LOG`
record per insert. That identifies the storage area, but not whether those page
images are avoidable overhead or required evidence for the ownerless native
history WAL fast path.

This slice attributes native-support page publication to the active
history-proof context. It is diagnostics-only; it does not elide any additional
page-version WAL records.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` acquires the ownerless page-write
  lock for the rollback-segment page, updates undo history, then arms
  `mylite_ownerless_history_proof_active` around the commit mini-transaction.
- The same function records the rollback-segment page
  (`mylite_ownerless_history_proof_rseg_page_no`) and undo-header page
  (`mylite_ownerless_history_proof_undo_page_no`) before `mtr->commit()`.
  The fast path is considered proved only when both pages are published at a
  nonzero commit LSN and no page-publish failure occurred.
- If that proof is not established, `write_serialisation_history()` falls back
  to `mylite_ownerless_innodb_flush_history_pages_to_lsn()` for exact native
  history-page flushing before releasing the ownerless history lock.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_can_elide_native_support_page()` deliberately refuses
  to elide native-support pages matching the active history-proof rollback
  segment or undo-header page. That protects the current proof contract.
- `ownerless_page_write_note_history_proof_page()` marks the proof only on a
  successful page-version publication, not on a native-support elision.
- `mariadb/storage/innobase/trx/trx0purge.cc`
  `trx_purge_add_undo_to_history()` updates both the rollback-segment page and
  undo-header page while moving the undo log to history/cached state.

## Design

Extend the existing ownerless page-publish stats with proof-context counters:

- native-support page publications that match the active history-proof rollback
  segment page;
- native-support page publications that match the active history-proof undo
  header page;
- native-support pages blocked from blind elision because they match the active
  history-proof rollback segment page;
- native-support pages blocked from blind elision because they match the active
  history-proof undo header page.

The counters are active only when page-publish stats are enabled. They are
reported through the existing stats array used by the embedded performance
probe and focused ownerless SQL tests.

## Scope And Non-Goals

In scope:

- Attribute the remaining published native-support undo-space pages to the
  ownerless history-proof contract.
- Preserve all existing page-publish and SYS identity counters.
- Add focused test assertions proving the new proof-context counters are
  internally consistent with the published native-support totals.
- Record production probe evidence in the performance docs.

Out of scope:

- Replacing the history-proof page-version publication with a different proof.
- Eliding the proof rollback-segment or undo-header page images.
- Changing undo history, purge, redo, checkpoint, or recovery behavior.
- SQL-level table-lock fault injection, broader DDL/file lifecycle recovery,
  external MariaDB/RQG stress, or WordPress PHPUnit workflow changes.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, storage-format, or public directory
layout behavior changes. The slice adds diagnostics and test assertions only.

## Database Directory And Lifecycle Impact

No durable file changes. The new counters are process-local and reset by the
existing page-publish stats reset function.

## Native Storage Impact

No native InnoDB page format or recovery behavior changes. If the counters
show the remaining native-support publications are history-proof pages, a later
optimization must first replace that proof with evidence that is equally strong
for peer visibility, forced `.shm` rebuild, crash recovery, and checkpoint
reconciliation.

## Build, Size, And Dependency Impact

No new dependency. Binary impact is limited to a small proof-kind helper and
four stats counters behind the existing ownerless diagnostics path.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive with the production `MinSizeRel`
  baseline after editing InnoDB source.
- Rebuild `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run focused ownerless page-publish selectors, especially
  `single-owner-history-wal-proof` and
  `single-owner-native-support-page-wal-elision`.
- Run a reduced stats-enabled production embedded performance probe and record
  the proof-context page counts per insert.
- Run a stats-off production embedded performance probe as a sanity check.
- Run production build guards, CI-production audit, format check, and
  whitespace check.

## Verification Results

Local verification on 2026-06-10 used `build/mariadb-embedded` with the
production `MinSizeRel` baseline and `build/php-embedded-prod` with
first-party `Release` artifacts:

- `tools/mariadb-embedded-build build` rebuilt the embedded MariaDB archive.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- Focused production CTest selectors
  `libmylite.ownerless-single-owner-history-wal-proof` and
  `libmylite.ownerless-single-owner-native-support-page-wal-elision` passed.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. It reported
  `1.000` published history-proof rollback-segment page and `1.000` published
  history-proof undo page per ownerless autocommit insert. Those matched the
  remaining `1.000` published `FIL_PAGE_TYPE_SYS` page and `1.000` published
  `FIL_PAGE_UNDO_LOG` page per insert. Both proof pages were also counted as
  blocked from blind native-support elision by the active proof gate.
- The same reduced sample reported ownerless autocommit at `707.05 ops/s`
  versus ordinary autocommit at `1884.01 ops/s`, `3.000` MTR-published page
  versions per insert, `3.570` native-support records per insert, `1.570`
  native-support elisions per insert, `49672.960` page-log bytes per insert,
  page-log append at `0.080 ms/insert`, commit-MTR publish at
  `0.177 ms/insert`, and write-history at `0.278 ms/insert`.
- A default stats-off production probe passed and reported ordinary
  autocommit at `1919.23 ops/s`, ownerless autocommit at `644.07 ops/s`,
  ordinary transactional inserts at `1381.87 ops/s`, ownerless transactional
  inserts at `837.62 ops/s`, and ownerless active-runtime reconnect at
  `1.217 ms`.

## Acceptance Criteria

- Focused tests pass and assert that proof-context publication counters are
  nonzero for the single-owner ownerless insert path.
- The reduced stats-enabled probe identifies whether the remaining published
  `FIL_PAGE_TYPE_SYS` and `FIL_PAGE_UNDO_LOG` pages match the history-proof
  rollback-segment and undo-header pages.
- No new page-version WAL elision or compatibility behavior change is
  introduced.
- CI timing-producing jobs remain guarded by production build checks.

## Risks And Unresolved Questions

- Attribution does not itself reduce runtime cost. If the pages are all proof
  pages, the next optimization target is the proof design, not blind
  native-support elision.
- Broader workloads may publish additional undo-space SYS pages outside this
  simple autocommit insert path; this slice measures the focused hot path and
  keeps broader DDL/native recovery classes planned.
