# Ownerless Capped Visible-Fast Batch

## Problem

Production performance probes show that ownerless process startup and reconnect
costs are close enough to ordinary embedded opens, while write-heavy paths
remain slower. The current CI-shaped bulk insert probe uses four-row
`INSERT ... VALUES` statements and reports five page-log append sessions for
each statement even though those statements already qualify for ownerless
visible-fast commit publication. That session churn adds page-log append lock,
header, and file-end work without reducing page-version WAL volume.

The existing visible-fast append batching slice intentionally limited the
append-session extension to single-row statements after a wider first attempt
regressed bulk timings. This follow-up keeps that guardrail by extending the
same behavior only to a small, parser-proven row-list cap that matches the
production probe shape.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` starts and ends a MyLite
  page-publish batch around ownerless dirty-page publication in
  `mtr_t::commit_log()`. Multi-row inserts can therefore close the append
  session after each mini-transaction even when SQL statement visibility is not
  published yet.
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` already parses pure
  `INSERT ... VALUES` row lists and rejects unsupported shapes such as upsert,
  `INSERT ... SELECT`, and trailing non-row tokens.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_allows_append_batch_fast_path()` uses a separate flag
  from the broader visible-fast commit path, so append-session lifetime can
  stay narrower than commit-visibility eligibility.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_pages_visible_hook()` releases any deferred append session
  before syncing the page-version WAL and publishing the page-visible LSN.
- `packages/libmylite/src/ownerless_page_log.cc`
  `mylite_ownerless_page_log_append_session_begin_initialized_at()` acquires
  the append byte-range lock and snapshots file identity/end state; keeping one
  session across a bounded statement removes repeated lock/header setup but
  does not change record encoding, checksums, or sync ordering.

## Design

Allow append-session deferral for parser-proven `INSERT ... VALUES` statements
with one through four row constructors. Keep all other statement shapes on the
existing behavior.

The cap is deliberately small:

- it covers the production performance probe's four-row bulk statement shape;
- it avoids holding the append lock across arbitrarily large SQL row lists;
- it preserves the unsupported upsert and broader DML exclusions already
  tested by the visible-fast append batching selector.

The page-version WAL format, history-proof native-support records, page-visible
LSN publication, checkpointing, and recovery semantics are unchanged.

## Scope And Non-Goals

In scope:

- direct and prepared ownerless `INSERT ... VALUES` statements whose parsed
  row-list count is between one and four and whose target table has no foreign
  keys;
- page-log append-session lifetime only;
- focused SQL coverage for a three-row visible-fast statement and the existing
  upsert negative branch;
- production performance evidence for bulk append-session counts.

Out of scope:

- `INSERT ... SELECT`, `REPLACE`, `UPDATE`, `DELETE`, DDL, `RETURNING`, upsert,
  generated fallback SQL shapes, and foreign-key target tables;
- group commit across SQL statements or processes;
- changing page-version WAL record format or shrinking history-proof page
  records;
- changing native InnoDB commit, undo, redo, or checkpoint behavior.

## Compatibility Impact

No public API, SQL result, storage format, or directory-layout behavior changes.
The same committed rows become visible at the same page-visible LSN. The change
only reduces process-local append-session lock/header churn before the normal
page-log sync and page-visible publication.

## Directory And Native Storage Impact

No new files are introduced. Durable page-version WAL and checkpoint state stay
inside the MyLite database directory. Native InnoDB page images, history proof
publication, redo, undo, and checkpoint behavior are unchanged.

## Build And Performance Impact

The code change is first-party MyLite statement classification and focused SQL
test coverage. It does not touch upstream-derived InnoDB files.

Expected probe signal for the default four-row bulk insert shape:

- page-version record counts remain stable;
- page-log append calls remain stable;
- page-log append session begin/end counts drop from several sessions per
  statement toward one session per statement;
- write throughput should improve if session setup was a meaningful part of
  the measured bulk cost, while larger native commit and history-proof costs
  remain open performance targets.

## Test And Verification Plan

- Extend `single-owner-multi-row-insert-visible-fast-path` to assert the
  existing three-row insert keeps one page-log append session while preserving
  visible-fast commit and history-proof publication.
- Keep the existing upsert negative branch on conservative commit visibility.
- Run the focused ownerless selector under `php-embedded-prod`.
- Run a production embedded performance probe with append stats and compare
  bulk page-log session counts.
- Run adjacent primitive/native-support/history-proof checks, production build
  guards, format checks, and `git diff --check`.

## Acceptance Criteria

- A capped multi-row visible-fast insert uses one page-log append session.
- Page-version publication, native history WAL proof, and visible commit
  publication remain covered.
- Unsupported upsert remains conservative.
- Production probe output shows lower bulk append-session churn without losing
  page-version records.

## Verification

Local verification on 2026-06-17 used production build artifacts:

- `cmake --build build/php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- `mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path` passed.
- `ctest --test-dir build/php-embedded-prod --output-on-failure -R
  'libmylite\.(ownerless-primitives|ownerless-single-owner-native-support-page-wal-elision|ownerless-single-owner-multi-row-insert-visible-fast-path|ownerless-insert-fk-fast-path-cache)$'`
  passed.
- A stats-enabled production performance probe with ownerless page-publish and
  page-log detail stats kept the default four-row bulk shape at `305` append
  calls, `6.100` append calls per statement, `6.000` page-version records per
  statement, and `2.000` native-support published pages per statement, while
  append-session begin/end calls dropped to `50`/`50` for `50` statements. The
  prior same-branch sample before this slice reported `250`/`250` begin/end
  calls for the same `50` statements.
- A stats-off production probe reported ownerless bulk rows at
  `4440.89 ops/s`, ownerless bulk statements at `1110.22 ops/s`, and
  ownerless/ordinary bulk ratio `0.6569` in that run. This is a local timing
  sample rather than a claim that the broader write path is complete.

## Risks And Follow-Up

The cap reduces but does not remove append-lock hold-time risk. Larger row-list
batching, broader DML batching, history-proof record replacement, and native
commit/write-history reduction require separate evidence.
