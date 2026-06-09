# Ownerless Single-Owner Undo Cache Reuse

## Problem

The ownerless undo-cache reuse profile shows simple ownerless autocommit DML is
blocked from MariaDB's one-page cached undo reuse on every measured row. A
100-row production attribution sample reported one ownerless cached-undo reuse
skip, one fresh undo-log create, one size-eligible history record, and one
ownerless-blocked cache-eligible history record per autocommit insert, with a
blocked-ownerless ratio of `1.0000`.

That identifies a concrete performance target, but enabling cached undo reuse
for all ownerless peers would be unsafe without a broader shared rollback
segment and purge protocol. MariaDB's `rseg->undo_cached` list is process-local.
If another ownerless process has been live, this process cannot assume its
cached undo list still matches on-disk rollback-segment history and slot state.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `trx_undo_assign()` and persistent `trx_undo_assign_low<false>()` normally
  call `trx_undo_reuse_cached()` before `trx_undo_create()`.
- `trx_undo_reuse_cached()` removes the first `trx_undo_t` from the
  process-local `rseg->undo_cached` list, latches its page, creates a new undo
  header with `trx_undo_header_create()`, and reinitializes the memory object
  with `trx_undo_mem_init_for_reuse()`.
- `trx_purge_add_undo_to_history()` places a committed one-page undo log into
  `rseg->undo_cached` only if it is below `TRX_UNDO_PAGE_REUSE_LIMIT`.
  Ownerless DML with a shared rw-trx registry element currently sends the same
  size-eligible undo log to `TRX_UNDO_TO_PURGE` instead.
- `trx_undo_mem_create_at_db_start()` reconstructs cached undo memory objects
  from on-disk `TRX_UNDO_CACHED` state, but that reconstruction is startup-time
  and does not keep a live process-local cached list synchronized after another
  ownerless process has been active.
- `ownerless_innodb_skip_external_page_refresh_hook()` in
  `packages/libmylite/src/database.cc` already defines the single-owner proof
  used by page-write and external-refresh skips. It requires mapped ownerless
  state, one active process, process-registry generation equal to the current
  process slot generation, zero active page-version pins, and a nonzero
  redo-visible or latest-LSN baseline.
- The process registry generation increments on both process-slot allocation
  and release. Therefore generation equality with this process slot proves no
  other ownerless process has joined or left since this process opened the
  directory; it is a continuous single-owner epoch, not merely a current
  `active_count == 1` observation.

## Design

Add a narrow InnoDB hook helper that returns whether ownerless may use the same
single-owner proof currently used to skip external page refresh. The helper will
call the existing skip-external-refresh callback and return success only when
that callback allows the skip.

Use that helper to allow cached undo reuse only under these conditions:

- non-ownerless hooks are not installed, preserving MariaDB's normal path; or
- ownerless hooks are installed and the single-owner proof allows external
  refresh skipping; or
- the transaction has no shared rw-trx registry element, preserving the current
  internal/recovered transaction exception.

In ownerless DML with live peers, prior peers, active snapshot pins, unmapped
state, or missing redo baseline, the current conservative behavior remains:
cached undo assignment is skipped and size-eligible history records are marked
for purge instead of placed into `rseg->undo_cached`.

No new durable format, shared-memory segment, or public API is added. The slice
only narrows when MariaDB's existing cached undo path is available in ownerless
mode.

## Compatibility Impact

No SQL, C API, PHP API, DDL, or wire-protocol behavior changes are intended.
The change should reduce ownerless autocommit overhead only in a continuous
single-owner ownerless epoch. Multi-process ownerless behavior stays
conservative until a broader shared rollback-segment cache protocol is designed.

## Native Storage Impact

No page, undo, redo, checkpoint, tablespace, or history-list format change.
When enabled by the single-owner proof, the native MariaDB cached-undo state is
written exactly as MariaDB already writes it: `TRX_UNDO_CACHED` history records
remain on the native history list and are reusable by the same process-local
rollback segment cache. When the proof is absent, ownerless keeps writing
`TRX_UNDO_TO_PURGE` for ordinary DML.

## Binary Size Impact

No new dependency. The implementation adds one small exported hook helper and
reuses the existing ownerless skip callback.

## Test Plan

- Rebuild the MariaDB embedded archive and production embedded performance
  probe.
- Run a reduced stats-enabled production embedded performance probe. In the
  single-owner sample, expect ownerless autocommit cached-undo attempts and
  hits, one initial miss/create, cached history records, and a blocked-ownerless
  ratio near zero.
- Run a stats-off production embedded performance probe and compare ordinary
  and ownerless write throughput to the previous branch samples.
- Run focused production ownerless commit/read/reclaim selectors.
- Run focused ownerless single-owner refresh-skip selectors to confirm the
  proof boundary remains covered.
- Run a focused stale-generation selector to confirm the single-owner proof
  blocks after another ownerless process has joined and left.
- Run focused hook crash-boundary selectors.
- Run production build-type guards, format check, and whitespace check.

## Acceptance Criteria

- Ownerless autocommit under a continuous single-owner epoch reuses cached undo
  logs in the stats-enabled probe without enabling reuse for live-peer or
  prior-peer cases.
- The probe shows cache attempts/hits and cached history records replacing the
  prior ownerless skip/create/blocked-history pattern for the single-owner
  sample.
- Focused ownerless correctness coverage still passes.
- Docs state that cached undo reuse remains limited to the single-owner proof
  and is not a general multi-process rollback-segment cache protocol.

## Risks And Follow-Up

- The single-owner proof intentionally blocks reuse after any peer has joined
  or left. This keeps the slice safe but means real multi-worker deployments may
  still see the conservative path while more than one process participates over
  a runtime's lifetime.
- A future broader optimization would need shared rollback-segment cache
  reconciliation, cross-process purge/cache ownership rules, and recovery tests
  before enabling cached undo reuse after peer activity.

## Verification Results

Local verification on 2026-06-09 used production embedded builds:

- `tools/mariadb-embedded-build build` passed with
  `build/mariadb-embedded` confirmed as `MinSizeRel`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed with `build/php-embedded-prod` confirmed as `Release`.
- The reduced stats-enabled production probe
  (`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`) passed. Ownerless autocommit
  reported cached-undo attempts at `1.000` per insert, cache-reuse hits at
  `0.810` per insert, cache-reuse misses and fresh creates at `0.190` per
  insert, ownerless cache-reuse skips at `0.000` per insert, history cached at
  `1.000` per insert, history-to-purge at `0.000` per insert, and an
  ownerless-blocked history-cache ratio of `0.0000`. The same sample reported
  ordinary autocommit at `856.48 ops/s` and ownerless autocommit at
  `312.83 ops/s`.
- The stats-off production probe passed and reported ordinary autocommit at
  `2151.77 ops/s`, ownerless autocommit at `374.23 ops/s`, and an ownerless
  autocommit ratio of `0.1739`; explicit ownerless transactions reported
  `1142.75 ops/s`.
- Under high host load during reruns, two default stats-off probe attempts
  aborted during the ordinary repeated warm-open phase with an InnoDB
  log-header checksum error. Minimal one-, two-, three-, and five-iteration
  repeated-open probes later passed, and a final default stats-off probe passed.
  Treat the aborted attempts as a remaining performance-harness stability
  caveat, not as passing evidence for this slice.
- Focused production ownerless SQL selectors passed: `commit-race`,
  `prepared-committed-read`, `local-write-first-read`, `live-reclaim`,
  `single-owner-page-write-refresh-skip`,
  `single-owner-external-refresh-skip`,
  `single-owner-foreground-reclaim-peer-history`, and
  `single-owner-skip-peer-history`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, followed by hook selectors
  `visible-publish-crash` and `visible-checkpoint-crash`.
- Production build guards passed for `build/mariadb-embedded`,
  `build/php-embedded-prod`, and `build/ownerless-test-hooks`.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.
