# Prepared Insert Checksum Phase Counters

## Problem

The prepared-insert component benchmark reports bind, step, reset, and commit
timings separately, but its storage checksum counters are aggregate counters
printed after commit and the final `SELECT COUNT(*)` verification. After the
dirty-refresh source counters and dirty-page copy cleanup, the aggregate output
still shows large dirty-page flush counts:

- dirty-page flush refreshes: `121,179`
- append-buffer flush refreshes: `6,849`
- dirty-page copy refreshes: `5,790`
- zero-tail checksum calls: `282,296`

Those totals mix insert-loop work, commit flush work, and post-commit
verification reads. The next prepared-insert optimization should distinguish
work that affects the timed `mylite_step()` loop from one-shot commit or
verification work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- The benchmark resets prepared-insert storage counters immediately after
  statement preparation in
  `tools/mylite_perf_baseline.c::benchmark_prepared_insert_components()`.
- The same function times the insert loop, finalizes the statement, times
  `COMMIT`, runs `SELECT COUNT(*)`, and only then prints aggregate storage
  counters.
- The existing storage test hooks expose aggregate checksum counts and dirty
  checksum refresh source counts, so the benchmark can snapshot them without
  new storage-layer hooks.

## Design

Add benchmark-only test-hook snapshots for prepared-insert checksum counters:

- capture checksum totals and dirty-refresh source counts after the prepared
  insert loop and statement finalize, before `COMMIT`;
- capture the same counters immediately after `COMMIT`;
- capture them again after the final row-count verification query;
- print a checksum phase table with full-page, zero-tail, and dirty-refresh
  totals for insert-loop, commit, and verification deltas; and
- print a dirty-refresh source delta table for the same three phases.

This slice does not change storage behavior or benchmark timings. It only
changes diagnostic output when storage test hooks are enabled.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route through the
same MyLite storage engine for `ENGINE=InnoDB`.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. The snapshots are process-local benchmark state.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to the benchmark tool. No dependency or license change.

## Test And Verification Plan

- Build the storage-smoke benchmark and confirm the prepared-insert component
  phase prints checksum phase and source-delta tables.
- Run the prepared-insert benchmark and record the insert-loop vs commit vs
  verification split.
- Keep storage and routed embedded storage-engine tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert benchmark output separates checksum totals by insert loop,
  commit, and verification phases.
- Dirty-refresh source counters are also reported as phase deltas.
- Existing aggregate prepared-insert storage counters remain available.
- The benchmark builds and the prepared-insert component phase passes.

## Verification

Verified on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`: passed;
  clang-format reported no modified files.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step measured `76.558 us/op`; prepared insert
  commit measured `55.299 ms`. The phase table reported `172,444` zero-tail
  calls and `131,049` dirty refreshes in the insert loop, `2,774` zero-tail
  calls and `2,773` dirty refreshes in commit, and `107,078` zero-tail calls
  with no dirty refreshes in verification. The source delta table showed all
  `121,179` dirty-page flush refreshes in the insert loop, while commit dirty
  refreshes came from `2,773` append-buffer flushes.

## Risks

The benchmark phase tables are diagnostic-only and machine-local. They do not
replace storage correctness tests when a later slice changes checksum
lifecycle behavior.
