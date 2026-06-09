# Ownerless History WAL Proof Fast Path

## Problem

Ownerless autocommit inserts still paid a native rollback-segment-space flush
after `trx_t::write_serialisation_history()`, even when the same mini-
transaction had already published the rollback-segment and undo-header pages to
the ownerless page-version WAL. Recent production attribution showed that this
flush was not falling back to a space scan; it was spending time writing and
waiting for the exact history pages. That made the next bounded optimization a
cheaper proof for those same pages, not broader checkpoint or doublewrite policy.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` mutates the rollback-segment page and
  undo-header page, commits the history mini-transaction, then calls
  `mylite_ownerless_innodb_flush_history_pages_to_lsn()` for the exact page
  proof when ownerless hooks hold the rollback-segment page-write lock.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_publish()` publishes dirty page images through
  `mylite_ownerless_innodb_publish_page_version()` during the same mini-
  transaction commit path.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_publish_page_version_hook()` appends a page-version WAL
  record for each published page image, while
  `ownerless_innodb_pages_visible_hook()` syncs the ownerless page WAL before
  advancing the visible page LSN.
- The existing ownerless visible-fast-path proof is limited to autocommit
  single-row `INSERT` statements. Native-support page WAL elision already uses
  that statement class and leaves dictionary, explicit transaction, and broader
  DML paths conservative.

## Design

Add a per-transaction history proof window for the ownerless history mini-
transaction. Before `mtr->commit()`, `trx_t::write_serialisation_history()`
records the expected rollback-segment tablespace id, rollback-segment page
number, and undo-header page number only when all of these are true:

- ownerless hooks are active;
- the rollback-segment page-write lock was acquired;
- the undo-header page is known;
- the transaction is not read-only or dictionary DDL;
- the SQL statement is autocommit single-row `INSERT`.

During `mtr_t::ownerless_page_write_publish()`, native-support page WAL elision
is disabled only for those exact expected history pages while the proof window
is active. Successful publication of those exact pages marks the rollback-
segment and undo-header proof flags. After `mtr->commit()`, the native exact
history flush is skipped only if:

- the proof window was allowed;
- the history MTR has a nonzero commit LSN;
- no page-version publish failed for the transaction;
- the expected rollback-segment page image was published;
- the expected undo-header page image was published.

Every other path keeps the existing native flush fallback.

## Scope And Non-Goals

In scope:

- A bounded fast path for ownerless autocommit single-row insert history pages.
- Focused SQL coverage proving the native history flush is skipped only after
  page WAL proof while native-support page publication remains active.
- Production attribution and stats-off throughput samples.

Out of scope:

- Broader redo/checkpoint reconciliation for native-support pages.
- DDL, dictionary, explicit transaction, multi-row DML, rollback, or recovery
  fast paths.
- SQL-level table-lock fault injection or external MariaDB/RQG stress.

## Compatibility Impact

No public SQL, C API, PHP API, mysqli, wire-protocol, metadata, or storage-
format behavior changes. The optimization is internal to ownerless InnoDB
commit proofing for one existing statement class. Unsupported and broader
statement classes stay on the native flush path.

## Directory And Lifecycle Impact

No new files or directory layout changes. The proof reuses the existing
ownerless page-version WAL and the existing visible-page LSN publication order.
If a process dies before visibility is advanced, the existing ownerless recovery
and native InnoDB recovery paths remain the authority.

## Native Storage Impact

The rollback-segment and undo-header native page images are still published to
the ownerless WAL. The slice avoids the additional native exact page flush only
after those exact images were accepted by the ownerless page-version hook. All
unproven paths still call
`mylite_ownerless_innodb_flush_history_pages_to_lsn()`.

## Build And Performance Impact

The MariaDB embedded archive must be rebuilt after the InnoDB source change.
CI timing remains production-build based:

- normal matrix jobs use the `prod` preset;
- embedded CTest, ownerless SQL, and embedded performance probes use
  `php-embedded-prod`;
- MariaDB embedded archives use `MinSizeRel`;
- WordPress PHP extension timing phases use
  `build/wordpress-php-embedded-prod` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` and the Release guard enabled.

The reduced production stats-enabled attribution sample on 2026-06-09 reported
ownerless autocommit at `729.55 ops/s`, `3.000` published page-version records
per insert, `3.570` native-support pages per insert, `1.570` native-support
elided pages per insert, `0.128 ms/insert` in page-log append,
`0.219 ms/insert` in write history, `0.000` ownerless history flush pages per
insert, and `0.000` exact history flush pages per insert. The companion
stats-off production sample reported ownerless autocommit at `775.42 ops/s`
versus ordinary autocommit at `2272.02 ops/s`, ratio `0.3413`.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive with `tools/mariadb-embedded-build
  build`.
- Build production embedded MyLite targets with
  `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`.
- Run focused CTest selectors for the history WAL proof and native-support page
  WAL elision.
- Run adjacent ownerless production selectors for primitives, single-owner
  refresh skips, and uncommitted peer visibility.
- Run a reduced stats-enabled production attribution probe.
- Run a reduced stats-off production throughput probe.
- Run ownerless unsafe-hook crash selectors through the production
  `ownerless-test-hooks` preset.
- Run `cmake --build --preset format`, `cmake --build --preset
  format-check-prod`, and `git diff --check`.

## Verification Results

Local verification on 2026-06-09 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel`, while `build/php-embedded-prod`,
`build/ownerless-test-hooks`, and `build/ownerless-stress` were `Release`.

- `tools/mariadb-embedded-build build` passed and rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-(history-wal-proof|native-support-page-wal-elision)$'
  --output-on-failure` passed.
- The adjacent production selector set passed for `ownerless-primitives`,
  `ownerless-single-owner-page-write-refresh-skip`,
  `ownerless-single-owner-external-refresh-skip`,
  `ownerless-single-owner-history-wal-proof`,
  `ownerless-single-owner-native-support-page-wal-elision`, and
  `ownerless-uncommitted-peer-hidden`.
- The final reduced stats-enabled production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` reported the zero-flush and
  native-support page evidence listed above.
- The final stats-off production throughput probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported the throughput sample listed
  above.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and
  `tools/require-cmake-release-build build/ownerless-test-hooks` accepted the
  hook cache. Hook selectors `visible-publish-crash` and
  `visible-checkpoint-crash` passed.
- `ctest --preset ownerless-stress --output-on-failure` passed the independent
  table stress case, then timed out once in `ownerless-cross-process-ddl-stress`
  at `900.20s`. Direct isolated reruns of `ddl-stress` with
  `MYLITE_OWNERLESS_DDL_STRESS_ROUNDS=1`, `4`, and `8` passed in `6.762s`,
  `8.887s`, and `21.731s`; the full stress rerun then passed DDL stress in
  `16.82s`.
- The full stress rerun later failed in
  `ownerless-cross-process-random-transaction-stress` at
  `run_ownerless_random_tx_stress_reader` with `sum >= previous_sum`.
  A temporary disabled-proof A/B, which forced
  `ownerless_history_wal_proof_allowed` false and rebuilt the stress binary,
  still reproduced the same random transaction stress assertion after three
  24-round passes. This failure is therefore not caused by the native history
  WAL proof fast path and remains separate ownerless stress debt.
- `ctest --preset ownerless-stress -E
  'libmylite\.ownerless-cross-process-random-transaction-stress$'
  --output-on-failure` passed the remaining 11 ownerless stress selectors.

## Acceptance Criteria

- Autocommit single-row insert coverage proves ownerless history native flush
  pages drop to zero.
- Native-support page WAL elision coverage still proves non-history support
  pages can elide while the exact history proof pages are published.
- The optimization is unavailable for unproven statement classes and all page
  publish failures fall back to the native exact history flush.
- Production stats-off throughput improves relative to the previous exact-
  flush samples without changing CI to non-production builds.

## Risks And Unresolved Questions

- The proof is intentionally narrow. Broader native redo/checkpoint
  reconciliation is still needed before native-support page publication can be
  reduced further.
- The ownerless performance gap is smaller but not closed; page-version append
  volume, commit-MTR publication, and clustered row insert time remain visible.
- External MariaDB/RQG stress, broader DDL/file lifecycle recovery, and SQL-
  level table-lock fault injection remain separate completion gaps.
