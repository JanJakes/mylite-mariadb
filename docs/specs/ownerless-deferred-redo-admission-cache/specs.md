# Ownerless Deferred Redo Admission Cache

## Problem

The rebuilt production page-write probe for the 16384-row ownerless bulk
insert shape still reports one logical ownerless redo written/leave event per
native mini-transaction. The existing visible-fast redo batching reduces shared
redo-state callbacks, but every `mtr_t::ownerless_redo_leave()` still calls
`mylite_ownerless_innodb_redo_defer_written_and_leave()` on the hot path.

That admission helper rechecked hook availability and callback context through
atomic loads for every mini-transaction even though deferred redo is scoped by
the statement-visible-fast/deferred-page-publish boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  calls `mylite_ownerless_innodb_redo_defer_written_and_leave()` from
  `mtr_t::ownerless_redo_leave()` before falling back to immediate
  written/leave completion.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  stores the statement deferred-page-publish flag in thread-local state and
  flushes pending deferred redo when the statement disables that flag.
- The same hook file flushes deferred redo before ownerless hooks are reset, so
  admitted ranges are already bounded by statement teardown and hook-reset
  boundaries.
- `packages/libmylite/src/database.cc`
  `OwnerlessStatementVisibleFastPathScope` enables deferred page publication
  only for statements that already allow visible-fast page publication and
  explicitly flushes deferred redo on scope teardown.

## Design

Cache deferred redo admission when a statement first enables deferred page
publication. The cached thread-local flag requires:

- ownerless lock hooks enabled,
- unsafe ownerless test faults disabled,
- a fused written/leave hook installed, and
- a non-null callback context.

The per-mini-transaction admission path then checks only top-level redo depth
and the cached flag before queueing a deferred range. Flush still loads the
current callback pointers at the statement/hook-reset boundary and preserves the
existing single-range fallback behavior when no batch hook is installed.

When deferred page publication is disabled, the implementation flushes pending
redo first and clears the cached admission flag.

## Compatibility Impact

No SQL behavior, C API, PHP API, native redo format, page-version WAL format,
checkpoint format, shared-memory format, or directory layout changes. The
change only removes repeated atomic hook/context checks from a statement-local
ownerless hot path.

Unsafe hook builds remain on the existing immediate path because admission is
cached as disabled while test faults are active.

## Native Storage Impact

Native InnoDB redo bytes, mini-transaction commit LSNs, ownerless redo
reservation lifetimes, page-visible publication order, and deferred flush
boundaries are unchanged.

## Test And Verification Plan

- Build `mylite_embedded_ownerless_innodb_lock_hooks_test`,
  `mylite_ownerless_cross_process_sql_test`, and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Extend the embedded hook test to prove deferred admission remains unavailable
  until the fused written/leave hook is installed, and that the separate
  written/leave fallback still clears the active redo state.
- Run focused visible-fast, history-WAL, native-support, and adjacent ownerless
  SQL selectors.
- Run a rebuilt production stats-enabled 16384-row page-write probe and compare
  redo-leave hook time and throughput against the rebuilt baseline.
- Run a rebuilt stats-off production bulk probe to confirm user-facing
  throughput does not regress.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Deferred redo admission uses a statement-scoped cached flag rather than
  reloading hook/context pointers on every mini-transaction.
- Missing fused written/leave hook coverage still returns unavailable and lets
  the caller use the immediate fallback.
- Deferred redo flush and hook reset cannot leave admitted ranges behind.
- Production probes show no throughput regression; keep the code only if the
  hot-path counters or stats-off throughput improve.

## Verification Results

Local production verification on 2026-06-21 used rebuilt embedded artifacts:

- `tools/mariadb-embedded-build build` rebuilt `libmariadbd.a` under the
  `MinSizeRel` embedded baseline.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  rebuilt the focused production targets.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
  passed.
- The production hook, history-WAL, and native-support selectors passed under
  `php-embedded-prod`; the grouped CTest run then timed out once in
  `ownerless-single-owner-multi-row-insert-visible-fast-path` at the CTest
  `90s` limit. The timeout left
  `/tmp/mylite-ownerless-sql.Erz6mr`, which was removed. A direct rerun of
  `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path` passed in about five
  seconds, and the adjacent `ownerless-insert-fk-fast-path-cache` and
  `ownerless-uncommitted-peer-hidden` CTest selectors passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|ownerless-history-proof-publish-failure-fallback|ownerless-stale-drop-crash-recovery)$'
  --output-on-failure` passed.

The rebuilt pre-slice stats-off baseline used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=32768
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=16384
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It reported ordinary remaining bulk at `185912.88 rows/s`, ownerless
remaining bulk at `59167.85 rows/s`, and an ownerless/ordinary remaining ratio
of `0.3182`.

The same stats-off probe after the admission cache first reported ordinary
remaining bulk at `166639.21 rows/s`, ownerless remaining bulk at
`69752.04 rows/s`, and an ownerless/ordinary remaining ratio of `0.4186`.
A final stats-off sample after reset/fault-guard cleanup reported ordinary
remaining bulk at `129284.82 rows/s`, ownerless remaining bulk at
`55999.65 rows/s`, and an ownerless/ordinary remaining ratio of `0.4331`.

The final page-write attribution probe used the same row shape with
`MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1`. Compared with the rebuilt
page-write baseline, it reported:

- ownerless remaining page-write commit-log time:
  `61.937` to `52.214 ms/statement`;
- ownerless remaining redo-leave time:
  `20.520` to `12.627 ms/statement`;
- ownerless remaining redo-leave hook time:
  `16.629` to `9.003 ms/statement`;
- native redo write calls stayed at `0.000` per remaining statement;
- fallback hook calls stayed at `0.000` per remaining statement;
- logical written/leave events stayed at `32782.000` per remaining statement;
- held native-support publish skips stayed at `16448.000` per remaining
  statement.

The instrumented page-write throughput samples remained noisy and are not used
as the user-facing performance gate. The stats-off samples are the production
throughput evidence for this slice.

## Risks

The cache assumes ownerless hook installation is stable for the duration of a
statement. That matches the existing embedded lifecycle: hook reset flushes
deferred redo before clearing callbacks, and statement scope teardown flushes
before clearing the deferred-page-publish flag.
