# Ownerless Page-Type Undo Classification

## Problem

Production ownerless bulk-insert profiling showed that repeated row-list
inserts remain slower than the ordinary embedded path after the page-version WAL
append work was coalesced. The remaining hot path is the ownerless InnoDB
mini-transaction bookkeeping that classifies every modified page before page
write lock acquisition, deferred transaction publication, and release.

The existing ownerless undo-page classifier called the tablespace path fallback
before inspecting the page type. For ordinary user index and BLOB pages, that
can enter `fil_space_t::get()` and inspect the space file name even though the
MariaDB page type already proves the page is not an undo log page.

## Scope

In scope:

- Avoid path-based undo tablespace lookup for page types that MariaDB defines
  as user index or BLOB page types.
- Keep the existing known undo-space checks for configured undo spaces.
- Keep the existing path-based fallback for ambiguous metadata pages.
- Run focused visible-fast, native-support, primitive, and performance probes.

Out of scope:

- Changing page-write lock semantics, redo serialization, history proof
  publication, WAL format, checkpoint format, SQL eligibility, or row-list bulk
  policy.
- Broadening MariaDB's default-checked bulk insert path beyond upstream's
  empty-root case.

## Source Findings

MariaDB `fil0fil.h` defines `FIL_PAGE_UNDO_LOG` as the undo log page type and
defines `FIL_PAGE_INDEX`, `FIL_PAGE_RTREE`, `FIL_PAGE_TYPE_BLOB`,
`FIL_PAGE_TYPE_ZBLOB`, and `FIL_PAGE_TYPE_ZBLOB2` separately for index and BLOB
page families.

MyLite's ownerless `mtr0mtr.cc` defers normal user pages to transaction-level
page publication for visible-fast row-list statements, but keeps system and undo
pages on the native-support/history-proof path. The classifier therefore only
needs the path fallback when the space id is not already a known undo space and
the page type is not enough to prove a normal user page.

## Design

Split the existing undo-space classification into:

- `ownerless_space_id_is_known_undo_tablespace()`, covering MariaDB's configured
  and built-in undo space id checks without opening the `fil_space_t`;
- `ownerless_space_is_undo_tablespace()`, retaining the existing path-based
  fallback for ambiguous spaces.

`ownerless_page_write_is_undo_page()` now checks known undo space ids first,
then inspects the in-memory page type. If the type is a definite user index or
BLOB page, it returns false without the path lookup. If the type is
`FIL_PAGE_UNDO_LOG`, it returns true. Ambiguous pages still fall through to the
existing path-based tablespace check.

## Compatibility Impact

No SQL, C API, PHP API, storage-format, WAL-format, native-page, checkpoint, or
directory-layout change. The change only avoids an internal classification
lookup for page types that cannot be undo log pages.

## Test Plan

- Rebuild the embedded MariaDB archive because the slice touches InnoDB source.
- Build production `mylite_embedded_performance_probe`,
  `mylite_ownerless_cross_process_sql_test`, and
  `mylite_ownerless_primitives_test`.
- Run the focused single-owner visible-fast multi-row insert selector and
  adjacent native-support/history-proof selectors.
- Run the ownerless primitive selector and a production stats-off/stats-enabled
  bulk performance probe.
- Run production build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- Definite user index and BLOB pages avoid the undo path fallback.
- Ambiguous metadata pages still use the existing path-based undo-space
  fallback.
- Focused ownerless correctness coverage stays green.
- Production performance probe output does not regress the ownerless bulk path
  and continues to report visible-fast transaction publication.

## Verification

- `tools/mariadb-embedded-build build` rebuilt the MinSizeRel embedded MariaDB
  archive with the InnoDB source change.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision))$'
  --output-on-failure` passed 4/4.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
  passed.
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision))$'
  --output-on-failure` passed 4/4.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-stress$' --output-on-failure` passed.
- A production stats-enabled 100-row bulk attribution probe reported
  `mylite_perf_summary_ownerless_autocommit_bulk_mysql_query_ms_per_statement=3.918`,
  `page_write_commit_log_ms_per_statement=1.118`,
  `page_write_commit_log_no_dirty_page_publish_ms_per_statement=0.126`,
  `page_write_commit_log_no_dirty_page_leave_ms_per_statement=0.164`, and
  `mylite_perf_summary_ownerless_insert_autocommit_bulk_rows_ratio=0.2553`.
- Two stats-off production probes remained noisy and showed 100-row bulk ratios
  of `0.2179` and `0.2590`; this slice therefore claims only the attributed
  page-classification/path-lookup reduction, not end-to-end parity with the
  ordinary embedded path.
- `tools/check-ci-production-builds`, `cmake --build --preset
  format-check-prod`, and `git diff --check` passed.

## Follow-Up

The next high-impact performance target is redo leave/log-write volume. The
same attribution probe still reports about `162` ownerless redo-leave hook
calls per 100-row bulk statement, with
`page_write_commit_log_redo_leave_ms_per_statement=0.634`.
