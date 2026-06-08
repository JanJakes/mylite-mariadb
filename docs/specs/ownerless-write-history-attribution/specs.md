# Ownerless Write History Attribution

## Problem

Production ownerless autocommit probes show the remaining write-throughput gap
is dominated by InnoDB commit work below the MyLite statement wrapper. The
current deep counter reports `trx_commit_persist_write_history` as one large
bucket, which is not enough to decide whether the next safe optimization should
target ownerless rollback-segment page-write ownership, post-wait refresh,
rollback-segment history-list mutation, the commit mini-transaction, or the
ownerless dirty-page flush after that mini-transaction.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::commit_persist()` calls `write_serialisation_history(&mtr)` before
  `commit_in_memory(&mtr)` when the transaction has persistent redo/undo.
- `trx_t::write_serialisation_history()` assigns the transaction serialization
  number, appends undo history to the purge queue under the rollback-segment
  latch, commits the mini-transaction, and in MyLite ownerless mode may acquire
  an ownerless page-write lock for the rollback-segment history page, refresh
  externally visible pages after waiting, flush dirty pages for the committed
  history MTR, and release the ownerless page-write lock.
- `packages/libmylite/tests/embedded_performance_probe.c` already emits deep
  ownerless InnoDB commit counters and compact per-insert autocommit summaries,
  but it only exposes write-history as one total.

## Design

Add deep performance subcounters inside
`trx_t::write_serialisation_history()`:

- ownerless history-page lock acquisition,
- ownerless refresh after a waited history-page lock,
- rollback-segment latch acquisition,
- history-list serialization and undo queue mutation,
- write-history mini-transaction commit,
- ownerless dirty-page flush after the committed history MTR,
- ownerless history-page lock release.

The counters are active only when the existing
`mylite_ownerless_innodb_deep_perf` instrumentation is enabled. They do not
change lock ordering, page publication, redo, undo, purge, checkpoint, or
visibility behavior.

The embedded performance probe prints the detailed subcounters and adds
per-insert summary lines for the ownerless autocommit sample so CI logs can
show which write-history subphase dominates.

## Compatibility Impact

No SQL, C API, PHP API, directory layout, native storage format, or ownerless
coordination semantics change. This is instrumentation only.

## Native Storage Impact

No native InnoDB page or redo/undo format changes. The instrumentation brackets
existing MariaDB commit and rollback-segment operations without changing their
control flow.

## Binary Size Impact

No production dependency changes. The embedded MariaDB archive gains a small
number of existing-style atomic counter updates behind an opt-in perf flag.

## Test Plan

- Rebuild the production embedded performance probe and the MariaDB embedded
  archive if needed.
- Run a reduced production stats-enabled embedded performance probe and verify
  the new write-history summary keys are present.
- Run a focused ownerless SQL commit/visibility selector to ensure behavior is
  unchanged.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The existing `trx_commit_persist_write_history` total remains available.
- New detailed and per-insert summary keys identify write-history subphase
  cost.
- Focused production ownerless correctness coverage still passes.
- No unsupported ownerless capability or performance parity claim is added.

## Verification Results

Local verification on 2026-06-08 used the production `php-embedded-prod`
build after rebuilding the MariaDB embedded archive:

- `tools/mariadb-embedded-build build` passed and rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- A reduced stats-enabled probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=40`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. The post-format sample
  reported ownerless autocommit at `345.86 ops/s`, write history at
  `1.359 ms/insert`, history-list mutation at `0.136 ms/insert`,
  write-history MTR commit at `0.104 ms/insert`, ownerless dirty-page flush at
  `1.106 ms/insert`, ownerless history-page lock at `0.002 ms/insert`,
  ownerless release at `0.009 ms/insert`, and no measurable ownerless
  post-wait refresh or rollback-segment latch time.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  commit-race` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- Stats-enabled probes carry instrumentation overhead. Compare stats-off
  throughput separately before claiming an optimization.
- If ownerless dirty-page flush or MTR commit dominates, a follow-up slice must
  prove correctness before changing flush/checkpoint behavior.
