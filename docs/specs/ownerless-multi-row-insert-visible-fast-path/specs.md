# Ownerless Multi-Row Insert Visible Fast Path

## Problem

Ownerless write-performance work has narrowed the hot path to statements whose
modified InnoDB page images can be proven by the ownerless page-version WAL.
The current visible-fast-path and history WAL proof gates are limited to a
single `INSERT ... VALUES (...)` row even though MariaDB's insert grammar treats
pure multi-row `VALUES` inserts as the same insert statement family. Bulk
application inserts therefore remain on the conservative native dirty-page
flush bridge despite using the same page-version publication, visibility, and
history-proof machinery as repeated one-row inserts.

The existing MyLite SQL classifier also admits `INSERT ... ON DUPLICATE KEY
UPDATE` and `INSERT ... RETURNING` after a single row. Those shapes can have
update-like or result-producing behavior and should not share the fast path
until separately proven.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB's INSERT documentation describes the `VALUES` form as
  `{VALUES | VALUE} ({expr | DEFAULT},...),(...),...`, with optional
  `ON DUPLICATE KEY UPDATE` and `RETURNING` clauses. The documented
  `INSERT ... SELECT` form is a separate production
  (<https://mariadb.com/docs/server/reference/sql-statements/data-manipulation/inserting-loading-data/insert>).
- `packages/libmylite/src/database.cc`
  `ownerless_statement_allows_visible_fast_path()` tokenizes SQL before it
  reaches MariaDB and marks a statement-local flag for the ownerless InnoDB
  bridge. It currently rejects a comma after the first row, but returns true
  for `ON` and `RETURNING` after a single row.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `ownerless_sql_command_allows_visible_fast_path()` consumes only that
  statement-local MyLite flag. The commit bridge still requires a nonzero
  ownerless commit LSN, page-write ownership, no unproved deferred dirty page
  writes, no page publish failure, and at least one published page image before
  it publishes visibility without native dirty-page flush.
- `trx_t::write_serialisation_history()` uses the same statement-local flag for
  the ownerless history WAL proof. The native exact history flush is skipped
  only when the expected rollback-segment and undo-header page images were
  published and no page-version publish failure occurred.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_can_elide_native_support_page()` also consumes the
  same statement-local flag, but keeps history-proof pages published and only
  elides other native-support pages when the transaction is autocommit and
  non-dictionary.

## Design

Broaden the first-party MyLite statement classifier to accept only the pure
`INSERT ... VALUES (...), (...), ...` row-list form. After the top-level
`VALUE` or `VALUES` token, the classifier walks row groups with a small
parenthesis-depth state machine:

- expect a row-start `(`;
- allow any nested expression tokens inside the row parentheses;
- after the row closes, accept either a top-level comma followed by another
  row, a statement terminator, or end of input;
- reject any other top-level token, including `ON`, `RETURNING`, and `SELECT`.

Record a transaction-local deferred-page proof bit in `trx_t`. The
transaction-page publisher attempts the union of transaction-owned and
transaction-dirtied non-gate pages, but the proof requires only dirtied
non-gate pages to receive a page-version record from a captured transaction
image or buffer-pool publish. Transaction-owned page-write lock vectors are
left intact so commit cleanup can release ownerless page-write locks through
the existing transaction lifecycle.

In the commit bridge, transaction-deferred pages block visible-only publication
only when their dirtied page images remain unproved after the transaction-page
publisher runs. This keeps pure multi-row inserts eligible when their deferred
dirty pages are page-WAL-proven, while preserving the conservative native flush
fallback for every unproved page class.

## Scope And Non-Goals

In scope:

- Pure autocommit `INSERT ... VALUES (...), (...)` statements.
- The `VALUE` synonym as the existing classifier already recognizes it.
- Positive ownerless coverage proving multi-row inserts can take the
  visible-only path and skip native exact history flush after page WAL proof.
- Negative coverage proving `INSERT ... ON DUPLICATE KEY UPDATE` stays on the
  conservative unproven-statement bridge.

Out of scope:

- `INSERT ... SELECT`, `INSERT ... SET`, `INSERT ... RETURNING`, `REPLACE`,
  `UPDATE`, `DELETE`, `LOAD DATA`, explicit transactions, DDL, dictionary
  operations, or randomized statement-shape expansion.
- Reducing history-proof page-version records or changing native redo,
  checkpoint, reclaim, or page-log append semantics.
- SQL-level table-lock fault injection and external MariaDB/RQG stress.

## Compatibility Impact

No SQL syntax, public C API, PHP API, mysqli API, storage format, or directory
layout changes. Supported SQL behavior remains inherited from MariaDB. The
change only selects a faster ownerless visibility implementation for a broader
subset of already-supported autocommit insert statements.

`INSERT ... ON DUPLICATE KEY UPDATE` and `INSERT ... RETURNING` become
intentionally conservative for the ownerless visibility proof. If they were
previously taking the fast path through the classifier shortcut, that behavior
was undocumented and not covered by correctness evidence.

## Directory And Lifecycle Impact

No new files, persistent metadata, runtime roots, or cleanup rules. The
existing ownerless page-version WAL, visible-LSN publication, checkpoint anchor,
peer-open, close, and no-live reopen paths remain the proof and recovery
authority.

## Native Storage Impact

Native InnoDB dirty pages for pure multi-row insert commits may remain dirty
after the ownerless visible-only commit path, exactly as they already can for
the proven one-row insert path. Peer readers and no-live recovery must continue
to use the ownerless page-version WAL until native checkpoint or reclaim proves
the native files cover the visible LSN. All rejected statement shapes and
accepted statements with unproved deferred dirty pages retain the native
dirty-page flush bridge.

## Build And Performance Impact

The default non-ownerless path is unchanged. The ownerless SQL classifier does
slightly more token-state work only for insert statements that already reached
the fast-path candidate check. Multi-row inserts can now amortize ownerless
commit visibility work across several inserted rows rather than forcing a
native dirty-page flush for the whole statement.

This slice does not reduce page-version append volume, history-proof native-
support page publication, or clustered-row insert cost. Those remain the next
larger ownerless write-performance targets.

## Test And Verification Plan

- Build production embedded targets:
  `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`.
- Add a focused SQL selector for pure multi-row insert fast-path coverage.
  The test should:
  - enable commit-visibility, page-publish, and deep InnoDB stats;
  - execute a pure multi-row `INSERT ... VALUES (...), (...), (...)`;
  - assert the visible-fast-path counter increments;
  - assert native history flush and exact history flush counters stay zero;
  - assert transaction-deferred page publication counters report page-version
    publication for the insert;
  - assert history-proof rollback-segment and undo page images were published;
  - verify row count and aggregate values through the same ownerless handle,
    a reopened ownerless handle, and a no-live native reopen after removing
    the ownerless shared-memory file.
- Add or extend the same selector with an `INSERT ... ON DUPLICATE KEY UPDATE`
  guard that resets commit-visibility stats, executes an upsert, and asserts
  the unproven-statement flush reason increments while the fast counter remains
  zero for that statement.
- Run adjacent focused selectors for the existing one-row history proof and
  native-support page WAL elision.
- Run production ownerless hook/stress selectors that exercise visible
  publication, checkpoint publication, and active-reader pressure as needed.
- Run production-build guards, format checks, and `git diff --check`.

## Verification Results

Local verification on 2026-06-12 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel` and `build/php-embedded-prod` was
`Release`.

- `tools/mariadb-embedded-build build` passed after the InnoDB proof-bit
  change and rebuilt `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed after rebuilding against the updated embedded archive.
- The focused direct selector
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path` passed. Its diagnostic run
  before removing temporary output reported one visible-fast commit, zero flush
  commits, zero unproven/deferred/publish-failed/no-published flush reasons,
  zero ownerless history flush pages, zero exact history flush pages, two
  transaction-image publish attempts with two published, two buffer publish
  attempts with one published, and zero retry attempts.
- Focused production CTest passed for
  `libmylite.ownerless-single-owner-history-wal-proof`,
  `libmylite.ownerless-single-owner-native-support-page-wal-elision`, and
  `libmylite.ownerless-single-owner-multi-row-insert-visible-fast-path`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and direct hook selectors
  `visible-publish-crash` and `visible-checkpoint-crash` passed.
- `ctest --preset ownerless-stress -R
  'libmylite\.ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)$|active-reader'
  --output-on-failure` passed the adjacent history/native-support and
  active-reader pressure selectors that matched that preset. After reconfiguring
  the stress preset, the direct stress-built
  `single-owner-multi-row-insert-visible-fast-path` selector also passed.
- Production-build guards passed:
  `tools/check-ci-production-builds`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/ownerless-test-hooks`, and
  `tools/require-cmake-release-build build/ownerless-stress`.
- `cmake --build --preset format-check-prod` with the pinned clang-format
  library path passed, and `git diff --check` passed.
- A reduced stats-enabled production performance probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported ordinary autocommit at
  `3560.94 ops/s`, ownerless autocommit at `1246.66 ops/s`, ratio `0.3501`,
  `200` ownerless autocommit visible-fast commits, zero ownerless autocommit
  flush commits, zero deferred/unproven/publish-failed/no-published flush
  reasons, `3.000` page versions per insert, `2.000` native-support published
  pages per insert, `1.000` native-support elided pages per insert, and
  `0.000` actual snapshot-boundary pages per insert.

## Acceptance Criteria

- Pure multi-row `INSERT ... VALUES` statements are admitted to the ownerless
  visible fast path only when the existing runtime page-publication proof gates
  also pass.
- `INSERT ... ON DUPLICATE KEY UPDATE` is rejected by the classifier and
  observed through the conservative unproven-statement flush counter.
- Existing one-row insert, history WAL proof, and native-support page elision
  coverage still passes.
- Docs and compatibility notes describe the expanded fast-path scope and the
  still-conservative statement classes.
- The diff remains first-party except for tests/docs/CMake wiring, with no new
  dependencies or binary-size-sensitive profile changes.

## Risks And Unresolved Questions

- Multi-row insert can touch more pages than a one-row insert. This slice relies
  on the existing runtime proof gates to fall back whenever any page-write
  candidate is deferred, skipped, or failed rather than trying to prove every
  possible row-level shape statically.
- `INSERT IGNORE` remains part of the accepted pure `VALUES` statement family
  when it has no `ON DUPLICATE KEY UPDATE` or `RETURNING` tail. More detailed
  warning/error-path stress can be added separately if application coverage
  shows a gap.
- The ownerless performance gap is not closed by this slice. It improves bulk
  insert statement selection while leaving page-log append volume and history-
  proof publication as the larger remaining optimization problem.
