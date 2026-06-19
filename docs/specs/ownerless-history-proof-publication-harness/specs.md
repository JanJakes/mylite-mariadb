# Ownerless History Proof Publication Harness

## Problem

Ownerless write performance is still dominated by the current history-proof
contract: simple visible-fast inserts publish one rollback-segment proof page
and one undo-header proof page before `trx_t::write_serialisation_history()`
can skip the conservative exact native history flush. Existing attribution
already identifies those pages, but the proof harness should be strict enough
to protect the next proof-replacement design.

This slice tightens tests and docs only. It does not change production proof
acceptance, WAL format, page publication, checkpointing, or recovery.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` accepts the ownerless history WAL
  proof only when the commit LSN is nonzero, no page publish failed, and both
  the rollback-segment and undo-header proof pages were published.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_can_elide_native_support_page()` blocks blind native
  support elision for active proof pages, and
  `ownerless_page_write_note_history_proof_page()` marks proof success only
  after accepted page-version publication.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_page_publish_hook()` appends native-support page-version
  records but deliberately skips live page-index publication for those records.
- The unsafe hook selector
  `history-proof-publish-failure-fallback` already forces native-support page
  publication failure and verifies committed visibility through fallback.

## Design

Tighten the focused SQL harness:

- the fast-path history proof asserts zero ownerless exact history flush pages
  and zero fallback rounds;
- accepted rollback-segment proof samples must match the published
  history-proof rollback-segment counter;
- accepted undo proof samples must match the published history-proof undo
  counter;
- published history-proof rollback-segment pages must account for the
  published native-support `FIL_PAGE_TYPE_SYS` pages in the controlled insert
  shape;
- published history-proof undo pages must account for the published
  native-support `FIL_PAGE_UNDO_LOG` pages in the same controlled shape;
- published native-support records must be accounted for by the live page-index
  skip counter;
- the unsafe failure selector must prove zero accepted proof samples and
  positive native history flush pages after publish failure.

## Compatibility Impact

No SQL behavior, public C API, PHP/mysqli behavior, wire-protocol behavior,
storage format, checkpoint format, directory layout, or recovery behavior
changes. This is a harness-only guard for future history-proof replacement
work.

## Directory And Lifecycle Impact

No new durable files, shared-memory fields, or cleanup paths are introduced.

## Native Storage Impact

No native InnoDB page, redo, undo, purge, or checkpoint behavior changes.

## Build And Performance Impact

Only focused test assertions and docs change. The production runtime is
unchanged. The slice should not require rebuilding the MariaDB embedded archive
unless the local build considers dependent test targets stale.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused history-proof and native-support selectors.
- Build the unsafe hook test target and run
  `history-proof-publish-failure-fallback`.
- Run a reduced stats-enabled production performance probe to preserve current
  proof counters and page-index skip attribution.
- Run production build guards, format check, and `git diff --check`.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision)$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test`
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  history-proof-publish-failure-fallback`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-primitives$'
  --output-on-failure`

The first unsafe fallback run failed after an attempted assertion that the
newer exact-flush subcounter must be positive for this forced publish-failure
shape. The run proved the broad native history flush counter is the guaranteed
fallback signal here, so the final harness asserts positive native history
flush pages, zero accepted proof samples, and conservative commit visibility.
After removing the over-specific exact subcounter assertion, the selector
passed.

The reduced stats-enabled production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed and reported for the 100-row ownerless autocommit insert phase:

- published native-support pages: `200`;
- published history-proof rollback-segment pages: `100`;
- published history-proof undo pages: `100`;
- published native-support `FIL_PAGE_TYPE_SYS` pages: `100`;
- published native-support `FIL_PAGE_UNDO_LOG` pages: `100`;
- rollback-segment proof samples: `100`;
- undo proof samples: `100`;
- rollback-segment proof identity unique/duplicate/overflow: `100` / `0` /
  `0`;
- undo proof identity unique/duplicate/overflow: `100` / `0` / `0`;
- ownerless native history flush pages: `0`;
- ownerless exact native history flush pages: `0`;
- ownerless exact native history fallback rounds: `0`;
- page-index native-support skip summary: `2.030` per insert.

The same reduced probe preserved the four-row bulk shape at `2.000` page
versions and `2.000` native-support proof pages per statement, with
`1.000` visible-fast commits and `0.000` visible-flush commits per statement.

## Acceptance Criteria

- The fast-path selector proves history proof samples, native-support page
  classes, and page-index skip counters agree for the controlled insert shape.
- The unsafe fallback selector proves a failed native-support publication
  produces no accepted proof samples and takes the native history flush
  fallback.
- Docs continue to mark production proof replacement as planned work.

## Risks And Follow-Up

- The strict role/type equality applies to the controlled simple insert shape,
  not to arbitrary DML/DDL.
- This is not a speedup. The next production performance slice still needs a
  design that replaces the current two-page proof with equally strong
  redo/checkpoint/recovery evidence.
