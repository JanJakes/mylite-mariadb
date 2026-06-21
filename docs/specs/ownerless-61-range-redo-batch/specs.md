# Ownerless 61 Range Redo Batch

## Problem

The ownerless visible-fast redo path currently batches deferred redo completion
in `48`-range chunks. The latest green CI run on `0c5fb0b` shows startup/open
close near parity, while ownerless write throughput remains the weaker signal.
For the common 100-row bulk probe, page-write attribution still reports roughly
`181` logical redo written/leave events per statement, so a `48`-range cap
forces four shared redo-state batch callbacks for that page-write subset where a
bounded larger cap can use three; the total database-level statement shape still
lands near four callbacks in the accepted attribution probe.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_redo_leave()` admits deferred written/leave completion only
  through the existing visible-fast deferred page-publish path.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc` stores
  deferred ranges in a fixed thread-local array and flushes them through the
  batch written/leave hook before page-visible publication.
- `packages/libmylite/src/ownerless_redo_state.cc` stores active-owner entries
  and active redo reservations in the same `64` shared slots. A full deferred
  batch therefore must leave room for the writer's active-owner entry and for a
  newly arriving peer to enter and reserve one redo range.

## Design

Raise `MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES` from `48` to `61`.

The cap is intentionally not `64`: `61` deferred reservations plus the current
owner entry consume `62` slots, leaving two slots for a peer active-owner entry
and first redo reservation if a peer opens after the single-owner visible-fast
proof. A static assertion in the InnoDB hook layer records that headroom, and
primitive coverage proves the full configured batch still allows a second owner
to enter, reserve, complete, and leave before the original batch flushes.

This does not alter native redo bytes, commit LSN assignment, page-version WAL,
page-visible publication order, shared-memory layout, checkpoint format, SQL
behavior, public C API behavior, or PHP API behavior.

## Verification Plan

- Build production hook, primitive, SQL, and performance probe targets.
- Run the embedded InnoDB hook test to prove a full `61`-range deferred batch
  uses one batch callback and reports the full completed count.
- Run ownerless primitive coverage to prove the full configured batch leaves peer
  active-entry and reservation headroom in the shared redo-state table.
- Run focused visible-fast and native-support/history-proof SQL selectors.
- Run a reduced production page-write attribution probe and compare callback
  counts against the prior `48`-range shape.
- Run production guards, format check, and whitespace check.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test
  mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(ownerless-primitives|embedded-ownerless-innodb-lock-hooks)$'
  --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-primitives|ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision))$'
  --output-on-failure`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|cross-process-checksum-stress)$'
  --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The accepted stats-enabled page-publish attribution probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=1000
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported:

- database redo written callbacks: `39` across ten 100-row bulk statements;
- database redo leave callbacks: `38` across ten 100-row bulk statements;
- logical page-write written-hook events: `181.800` per statement;
- page-version records: `2.000` per statement;
- page-log append calls: `32.300` per statement;
- page-write commit-log redo-leave time: `0.090 ms/statement`;
- page-write redo hook time: `0.071 ms/statement`;
- ownerless bulk `mysql_query()`: `3.177 ms/statement`.

The companion page-write attribution probe preserved the same logical
page-write event shape and reported `0.048 ms/statement` aggregate page-write
redo-hook time. Two stats-off 5000-row, 100-row-per-statement local samples
reported noisy ownerless/ordinary bulk ratios of `0.1944` and `0.1833`, with
unrelated open/close variance in the second run. This slice therefore records a
bounded callback/progress-latch reduction rather than a broad throughput claim.

## Acceptance Criteria

- A full deferred batch carries `61` ranges to the batch callback.
- The batch cap preserves at least two shared redo-state slots beyond the active
  writer's owner entry and configured deferred reservations.
- Visible-fast SQL coverage still proves page-visible publication follows
  deferred redo completion.
- Production attribution shows fewer database-level redo written/leave callback
  invocations for the same 100-row bulk shape while preserving logical page-write
  redo event volume.

## Risks

Larger batches keep redo reservations active for longer inside a visible-fast
statement. The cap is bounded by explicit peer headroom and remains scoped to the
single-owner visible-fast deferred publication path.
