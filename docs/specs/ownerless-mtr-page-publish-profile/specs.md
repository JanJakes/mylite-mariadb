# Ownerless MTR Page Publish Profile

## Problem

Production-built probes now separate CI timing phases and show two different
performance costs:

- WordPress/PHPUnit process isolation pays the embedded MariaDB process
  lifetime cost, mostly `mysql_server_init()` and `mysql_server_end()`.
- Ownerless autocommit inserts remain slower than ordinary autocommit inside
  `mysql_stmt_execute()`, even after page-log header validation and adapter
  overhead were reduced.

The existing ownerless counters show page-version append volume and outer
publish-hook time, but they do not explain the mini-transaction work around
page publication. A safe optimization needs to know whether time is spent in
MTR memo scanning, transaction deferral, page-image copy/checksum, the
first-party page-version hook, or the surrounding InnoDB commit-log phases.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::commit_log()` is the mini-transaction commit path that writes page
  LSNs, queues dirty pages on the flush list, leaves ownerless redo, publishes
  ownerless page images, releases memo slots, and may publish inline in the
  no-flush-list branch.
- `mtr_t::ownerless_page_writes_publish()` scans the MTR memo, defers
  transaction-published pages, and directly calls
  `mtr_t::ownerless_page_write_publish()` for page classes that cannot wait for
  transaction visibility.
- `mtr_t::ownerless_page_write_publish()` looks up the InnoDB space, allocates
  an aligned page buffer, copies the buffer-pool image, prepares checksum/write
  state, verifies `FIL_PAGE_LSN`, and calls
  `mylite_ownerless_innodb_publish_page_version()`.
- `packages/libmylite/tests/embedded_performance_probe.c` already enables and
  prints ownerless page-write perf stats when
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

## Design

Extend the existing opt-in ownerless page-write perf stats with phase counters
for:

- MTR publish scan calls and total scan time;
- transaction-deferred page count;
- direct page publish space lookup, allocation, copy, checksum, hook, and free
  time;
- ownerless MTR commit-log calls split by made-dirty and no-dirty branches;
- ownerless MTR commit-log total, flush-list, commit-log-release, redo-leave,
  publish, memo-release, and no-dirty-loop time.

The counters remain process-local relaxed atomics behind the existing
stats-enabled gate. Default ownerless execution and normal CI tests keep the
same behavior and do not emit the new fields.

## Compatibility Impact

No SQL, public C API, PHP API, native storage format, page-log format, lock
ordering, checkpoint, or recovery behavior changes. The new symbols are
internal diagnostics already exposed through the first-party performance probe.

## Directory And Lifecycle Impact

No durable file or directory-layout changes. The counters are reset between
the transactional and autocommit ownerless insert phases by the existing
performance probe.

## Native Storage Impact

Native InnoDB page publication and mini-transaction ordering are unchanged.
The slice only times existing work around InnoDB MTR commit and MyLite
ownerless page-version hooks.

## Build And Performance Impact

Stats-off runtime behavior is unchanged except for existing ownerless
performance-gate checks in instrumented paths. Stats-on probe runs perform more
steady-clock reads and relaxed atomic increments, which is acceptable for
diagnostic CI/local samples and should not be used as the exact non-probe
throughput baseline.

## Test Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build production embedded targets for the performance probe and focused
  ownerless SQL/primitive tests.
- Run a stats-enabled reduced production performance probe and confirm the
  new MTR publish and commit-log fields are emitted.
- Run focused ownerless correctness selectors that cover page visibility,
  reclaim, active-reader pressure, and commit-race behavior.
- Run production ownerless stress, production unsafe-hook negative proof,
  `format-check-prod`, and `git diff --check`.

## Verification Results

Local production verification on 2026-06-08:

- `tools/mariadb-embedded-build build` passed and rebuilt
  `libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test` passed.
- A reduced production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=400`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed and emitted the new MTR
  fields.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-primitives$' --output-on-failure` passed in `2.88s`
  on the final rerun.
- Direct focused production SQL selectors passed:
  `prepared-committed-read`, `local-write-first-read`, `native-reclaim`,
  `live-reclaim`, `commit-race`, and `active-reader-pressure` on the final
  rerun.
- `ctest --preset ownerless-stress --output-on-failure` passed all 12
  production stress cases in `144.77s`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure` passed all 3 unsafe-hook negative-proof cases in
  `20.52s`.
- `ctest --preset php-embedded-prod -LE compat.ownerless-cross-process-sql
  --parallel 2 --output-on-failure` passed all 48 production embedded
  non-ownerless cases in `58.49s`.
- The production direct ownerless SQL loop passed all `165` cases with `/tmp`
  ownerless SQL cleanup between cases.

The reduced ownerless autocommit insert sample reported:

- ownerless autocommit throughput: `217.63 ops/s`;
- `prepared_step_mysql_execute_ms=1513.851`;
- `page_write_commit_log_total_ms=312.267`;
- `page_write_commit_log_publish_ms=174.349`;
- `page_write_commit_log_release_memo_ms=96.815`;
- `page_write_commit_log_redo_leave_ms=39.779`;
- `page_write_publish_total_ms=172.780`;
- `page_write_publish_hook_ms=146.878`;
- `page_write_publish_alloc_ms=2.949`;
- `page_write_publish_copy_ms=11.521`;
- `page_write_publish_checksum_ms=7.366`;
- `page_log_append_total_ms=120.241`.

The measured MTR publish/commit path is significant, and the direct publish
subphase is dominated by the first-party page-version hook and page-log append.
However, MTR commit-log work does not explain most of
`mysql_stmt_execute()` in this sample. The next profiling slice should move
higher in the SQL/handler transaction path unless a bounded native-support
page-publication reduction can be proven safe independently.

## Acceptance Criteria

- The performance probe emits MTR publish subphase and commit-log timing
  fields under `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- Existing ownerless correctness tests and stress coverage still pass.
- Docs present the new counters as diagnostic evidence, not as a completed
  ownerless performance optimization.

## Risks And Follow-Up

- More counters can explain the slowdown but do not reduce it by themselves.
- If MTR commit-log total accounts for most of `mysql_stmt_execute()`, the next
  optimization should target native-support publication volume or batching with
  crash and active-reader evidence.
- If MTR commit-log total remains much smaller than `mysql_stmt_execute()`, the
  next profiling slice should move higher in the SQL/handler transaction path.
